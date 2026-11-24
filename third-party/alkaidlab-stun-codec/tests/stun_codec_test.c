#include "alkaidlab/network/stun_codec.h"

#include <openssl/sha.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(condition)                                                                                               \
  do {                                                                                                                 \
    if (!(condition)) {                                                                                                \
      fprintf(stderr, "FAIL: %s (line %d)\n", #condition, __LINE__);                                                   \
      failures++;                                                                                                      \
    }                                                                                                                  \
  } while (0)

static void write_be16(uint8_t *p, uint16_t value) {
  p[0] = (uint8_t)(value >> 8u);
  p[1] = (uint8_t)value;
}

static void write_be32(uint8_t *p, uint32_t value) {
  p[0] = (uint8_t)(value >> 24u);
  p[1] = (uint8_t)(value >> 16u);
  p[2] = (uint8_t)(value >> 8u);
  p[3] = (uint8_t)value;
}

static void test_xor_mapped_ipv4(void) {
  const uint8_t transaction_id[12] = {0x10, 0x11, 0x12, 0x13, 0x20, 0x21, 0x22, 0x23, 0x30, 0x31, 0x32, 0x33};
  uint8_t response[32] = {0};
  write_be16(response, 0x0101u);
  write_be16(response + 2u, 12u);
  write_be32(response + 4u, 0x2112A442u);
  memcpy(response + 8u, transaction_id, sizeof(transaction_id));
  write_be16(response + 20u, 0x0020u);
  write_be16(response + 22u, 8u);
  response[25] = 0x01u;
  write_be16(response + 26u, (uint16_t)(47989u ^ 0x2112u));
  write_be32(response + 28u, 0xC000020Au ^ 0x2112A442u);

  AlkStunMappedEndpoint endpoint;
  CHECK(alk_stun_decode_mapped_endpoint(response, sizeof(response), transaction_id, &endpoint));
  CHECK(endpoint.version == ALK_STUN_MAPPED_ENDPOINT_VERSION);
  CHECK(endpoint.address_family == ALK_STUN_ADDRESS_IPV4);
  CHECK(endpoint.port == 47989u);
  CHECK(endpoint.address[0] == 192u && endpoint.address[1] == 0u && endpoint.address[2] == 2u &&
        endpoint.address[3] == 10u);
  for (size_t length = 0u; length < sizeof(response); ++length) {
    CHECK(!alk_stun_decode_mapped_endpoint(response, length, transaction_id, &endpoint));
  }
}

static void test_xor_mapped_ipv6(void) {
  const uint8_t transaction_id[12] = {0x10, 0x11, 0x12, 0x13, 0x20, 0x21, 0x22, 0x23, 0x30, 0x31, 0x32, 0x33};
  const uint8_t address[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x12, 0x34};
  const uint8_t mask[16] = {0x21, 0x12, 0xa4, 0x42, 0x10, 0x11, 0x12, 0x13,
                            0x20, 0x21, 0x22, 0x23, 0x30, 0x31, 0x32, 0x33};
  uint8_t response[44] = {0};
  write_be16(response, 0x0101u);
  write_be16(response + 2u, 24u);
  write_be32(response + 4u, 0x2112A442u);
  memcpy(response + 8u, transaction_id, sizeof(transaction_id));
  write_be16(response + 20u, 0x0020u);
  write_be16(response + 22u, 20u);
  response[25] = 0x02u;
  write_be16(response + 26u, (uint16_t)(5349u ^ 0x2112u));
  for (size_t index = 0u; index < sizeof(address); ++index) {
    response[28u + index] = address[index] ^ mask[index];
  }

  AlkStunMappedEndpoint endpoint;
  CHECK(alk_stun_decode_mapped_endpoint(response, sizeof(response), transaction_id, &endpoint));
  CHECK(endpoint.address_family == ALK_STUN_ADDRESS_IPV6);
  CHECK(endpoint.port == 5349u);
  CHECK(memcmp(endpoint.address, address, sizeof(address)) == 0);
}

static void test_rejects_wrong_transaction(void) {
  const uint8_t transaction_id[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
  uint8_t response[32] = {0};
  write_be16(response, 0x0101u);
  write_be16(response + 2u, 12u);
  write_be32(response + 4u, 0x2112A442u);
  memset(response + 8u, 0xaau, 12u);
  write_be16(response + 20u, 0x0020u);
  write_be16(response + 22u, 8u);
  response[25] = 0x01u;
  AlkStunMappedEndpoint endpoint;
  CHECK(!alk_stun_decode_mapped_endpoint(response, sizeof(response), transaction_id, &endpoint));
}

static void test_integrity_and_fingerprint(uint32_t algorithm) {
  const uint8_t transaction_id[12] = {0x31, 0x32, 0x33, 0x34, 0x41, 0x42, 0x43, 0x44, 0x51, 0x52, 0x53, 0x54};
  static const char username[] = "remote:local";
  static const char password[] = "short-term-password";
  uint8_t message[512];
  AlkStunMessageBuilder builder;
  CHECK(alk_stun_builder_init(&builder, message, sizeof(message), ALK_STUN_BINDING_REQUEST, transaction_id));
  CHECK(alk_stun_builder_add(&builder, ALK_STUN_ATTR_USERNAME, username, (uint16_t)(sizeof(username) - 1u)));
  CHECK(alk_stun_builder_add_u32(&builder, ALK_STUN_ATTR_PRIORITY, 0x6e0001ffu));
  CHECK(alk_stun_builder_add_u64(&builder, ALK_STUN_ATTR_ICE_CONTROLLING, 0x0102030405060708ull));
  CHECK(alk_stun_builder_add_flag(&builder, ALK_STUN_ATTR_USE_CANDIDATE));
  CHECK(alk_stun_builder_add_integrity(&builder, algorithm, password, sizeof(password) - 1u));
  CHECK(!alk_stun_builder_add_u32(&builder, ALK_STUN_ATTR_PRIORITY, 1u));
  CHECK(alk_stun_builder_add_fingerprint(&builder));
  CHECK(alk_stun_message_validate(message, builder.length));
  CHECK(alk_stun_verify_integrity(message, builder.length, algorithm, password, sizeof(password) - 1u));
  CHECK(alk_stun_verify_fingerprint(message, builder.length));

  AlkStunAttributeView attribute;
  CHECK(alk_stun_find_attribute(message, builder.length, ALK_STUN_ATTR_USERNAME, &attribute));
  CHECK(attribute.length == sizeof(username) - 1u);
  CHECK(memcmp(attribute.value, username, attribute.length) == 0);
  CHECK(alk_stun_find_attribute(message, builder.length, ALK_STUN_ATTR_USE_CANDIDATE, &attribute));
  CHECK(attribute.length == 0u);

  uint8_t tampered[sizeof(message)];
  memcpy(tampered, message, builder.length);
  tampered[ALK_STUN_HEADER_BYTES + 4u] ^= 1u;
  CHECK(!alk_stun_verify_integrity(tampered, builder.length, algorithm, password, sizeof(password) - 1u));
  CHECK(!alk_stun_verify_fingerprint(tampered, builder.length));
  CHECK(!alk_stun_verify_integrity(message, builder.length, algorithm, "wrong", 5u));
}

static void test_rfc5769_short_term_request_vector(void) {
  /* RFC 5769 section 2.1 / Appendix A, reproduced under the RFC code license.
   */
  static const uint8_t request[] = {
      0x00, 0x01, 0x00, 0x58, 0x21, 0x12, 0xa4, 0x42, 0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34, 0xd6, 0x86, 0xfa, 0x87,
      0xdf, 0xae, 0x80, 0x22, 0x00, 0x10, 0x53, 0x54, 0x55, 0x4e, 0x20, 0x74, 0x65, 0x73, 0x74, 0x20, 0x63, 0x6c,
      0x69, 0x65, 0x6e, 0x74, 0x00, 0x24, 0x00, 0x04, 0x6e, 0x00, 0x01, 0xff, 0x80, 0x29, 0x00, 0x08, 0x93, 0x2f,
      0xf9, 0xb1, 0x51, 0x26, 0x3b, 0x36, 0x00, 0x06, 0x00, 0x09, 0x65, 0x76, 0x74, 0x6a, 0x3a, 0x68, 0x36, 0x76,
      0x59, 0x20, 0x20, 0x20, 0x00, 0x08, 0x00, 0x14, 0x9a, 0xea, 0xa7, 0x0c, 0xbf, 0xd8, 0xcb, 0x56, 0x78, 0x1e,
      0xf2, 0xb5, 0xb2, 0xd3, 0xf2, 0x49, 0xc1, 0xb5, 0x71, 0xa2, 0x80, 0x28, 0x00, 0x04, 0xe5, 0x7a, 0x3b, 0xcf,
  };
  static const char password[] = "VOkJxbRl1RmTxUk/WvJxBt";
  CHECK(alk_stun_message_validate(request, sizeof(request)));
  CHECK(alk_stun_verify_integrity(request, sizeof(request), ALK_STUN_INTEGRITY_SHA1, password, sizeof(password) - 1u));
  CHECK(alk_stun_verify_fingerprint(request, sizeof(request)));
}

static void test_xor_mapped_roundtrip(void) {
  const uint8_t transaction_id[12] = {1, 3, 5, 7, 9, 11, 13, 15, 17, 19, 21, 23};
  AlkStunMappedEndpoint expected;
  alk_stun_mapped_endpoint_init(&expected);
  expected.address_family = ALK_STUN_ADDRESS_IPV6;
  expected.port = 49152u;
  const uint8_t address[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 1, 0, 2, 0, 3, 0, 4, 0, 5, 0, 6};
  memcpy(expected.address, address, sizeof(address));
  uint8_t message[256];
  AlkStunMessageBuilder builder;
  CHECK(alk_stun_builder_init(&builder, message, sizeof(message), ALK_STUN_BINDING_SUCCESS_RESPONSE, transaction_id));
  CHECK(alk_stun_builder_add_xor_mapped_endpoint(&builder, &expected));
  CHECK(alk_stun_builder_add_fingerprint(&builder));
  AlkStunMappedEndpoint decoded;
  CHECK(alk_stun_decode_mapped_endpoint(message, builder.length, transaction_id, &decoded));
  CHECK(decoded.address_family == expected.address_family);
  CHECK(decoded.port == expected.port);
  CHECK(memcmp(decoded.address, expected.address, sizeof(address)) == 0);
}

static void test_rfc5780_diagnostic_attributes(void) {
  const uint8_t transaction_id[12] = {2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24};
  uint8_t request[128];
  AlkStunMessageBuilder request_builder;
  CHECK(alk_stun_builder_init(&request_builder, request, sizeof(request), ALK_STUN_BINDING_REQUEST, transaction_id));
  CHECK(alk_stun_builder_add_change_request(&request_builder, true, false));
  AlkStunAttributeView change;
  CHECK(alk_stun_find_attribute(request, request_builder.length, ALK_STUN_ATTR_CHANGE_REQUEST, &change));
  CHECK(change.length == 4u && change.value[3] == 0x04u);

  AlkStunMappedEndpoint origin;
  AlkStunMappedEndpoint other;
  alk_stun_mapped_endpoint_init(&origin);
  alk_stun_mapped_endpoint_init(&other);
  origin.address_family = ALK_STUN_ADDRESS_IPV4;
  origin.port = 3478u;
  origin.address[0] = 192u;
  origin.address[1] = 0u;
  origin.address[2] = 2u;
  origin.address[3] = 10u;
  other = origin;
  other.port = 3479u;
  other.address[3] = 11u;

  uint8_t response[256];
  AlkStunMessageBuilder response_builder;
  CHECK(alk_stun_builder_init(&response_builder, response, sizeof(response), ALK_STUN_BINDING_SUCCESS_RESPONSE,
                              transaction_id));
  uint8_t encoded_origin[8] = {0u, 1u, 0u, 0u, 192u, 0u, 2u, 10u};
  encoded_origin[2] = (uint8_t)(origin.port >> 8u);
  encoded_origin[3] = (uint8_t)origin.port;
  uint8_t encoded_other[8] = {0u, 1u, 0u, 0u, 192u, 0u, 2u, 11u};
  encoded_other[2] = (uint8_t)(other.port >> 8u);
  encoded_other[3] = (uint8_t)other.port;
  CHECK(alk_stun_builder_add(&response_builder, ALK_STUN_ATTR_RESPONSE_ORIGIN, encoded_origin, sizeof(encoded_origin)));
  CHECK(alk_stun_builder_add(&response_builder, ALK_STUN_ATTR_OTHER_ADDRESS, encoded_other, sizeof(encoded_other)));
  CHECK(alk_stun_decode_response_origin(response, response_builder.length, transaction_id, &origin));
  CHECK(alk_stun_decode_other_address(response, response_builder.length, transaction_id, &other));
  CHECK(origin.port == 3478u && origin.address[3] == 10u);
  CHECK(other.port == 3479u && other.address[3] == 11u);
}

static void test_long_term_key_derivation(void) {
  static const uint8_t expected_md5[16] = {
      0x84, 0x93, 0xfb, 0xc5, 0x3b, 0xa5, 0x82, 0xfb, 0x4c, 0x04, 0x4c, 0x45, 0x6b, 0xdc, 0x40, 0xeb,
  };
  static const uint8_t expected_sha256[32] = {
      0x07, 0xe9, 0x34, 0x11, 0x7a, 0xbd, 0x40, 0x83, 0x6e, 0x7c, 0x63, 0x29, 0xb5, 0x47, 0x31, 0xb2,
      0xb2, 0xd2, 0xa5, 0xf9, 0xa7, 0x1f, 0x54, 0x49, 0x22, 0xd7, 0x5e, 0x07, 0x30, 0xd8, 0x25, 0x1b,
  };
  uint8_t key[32];
  size_t key_length = 0u;
  CHECK(alk_stun_derive_long_term_key(1u, "user", 4u, "realm", 5u, "pass", 4u, key, sizeof(key), &key_length));
  CHECK(key_length == sizeof(expected_md5));
  CHECK(memcmp(key, expected_md5, sizeof(expected_md5)) == 0);
  CHECK(alk_stun_derive_long_term_key(2u, "user", 4u, "realm", 5u, "pass", 4u, key, sizeof(key), &key_length));
  CHECK(key_length == sizeof(expected_sha256));
  CHECK(memcmp(key, expected_sha256, sizeof(expected_sha256)) == 0);
  CHECK(!alk_stun_derive_long_term_key(2u, "user", 4u, "realm", 5u, "pass", 4u, key, 16u, &key_length));
}

static void test_turn_extension_primitives(void) {
  const uint8_t transaction_id[12] = {3, 6, 9, 12, 15, 18, 21, 24, 27, 30, 33, 36};
  AlkStunMappedEndpoint relayed;
  alk_stun_mapped_endpoint_init(&relayed);
  relayed.address_family = ALK_STUN_ADDRESS_IPV4;
  relayed.port = 50000u;
  relayed.address[0] = 192u;
  relayed.address[1] = 0u;
  relayed.address[2] = 2u;
  relayed.address[3] = 44u;
  uint8_t response[128];
  AlkStunMessageBuilder builder;
  CHECK(alk_stun_builder_init(&builder, response, sizeof(response), 0x0103u, transaction_id));
  CHECK(alk_stun_builder_add_xor_endpoint(&builder, 0x0016u, &relayed));
  CHECK(alk_stun_message_matches(response, builder.length, 0x0103u, transaction_id));
  AlkStunMappedEndpoint decoded;
  CHECK(alk_stun_decode_xor_endpoint(response, builder.length, transaction_id, 0x0016u, &decoded));
  CHECK(decoded.port == relayed.port);
  CHECK(memcmp(decoded.address, relayed.address, 4u) == 0);

  uint8_t error_response[128];
  CHECK(alk_stun_builder_init(&builder, error_response, sizeof(error_response), 0x0113u, transaction_id));
  const uint8_t unauthorized[] = {0u, 0u, 4u, 1u, 'U', 'n', 'a', 'u', 't', 'h'};
  CHECK(alk_stun_builder_add(&builder, 0x0009u, unauthorized, sizeof(unauthorized)));
  uint16_t error_code = 0u;
  CHECK(alk_stun_decode_error_code(error_response, builder.length, transaction_id, &error_code));
  CHECK(error_code == 401u);
}

/* RFC 8489 section 5: a Binding request is the 20-byte header and nothing else
 * (type 0x0001, length 0, the fixed magic cookie, the 96-bit transaction ID). */
static void test_rfc8489_binding_request_encoding(void) {
  static const uint8_t transaction_id[12] = {0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34, 0xd6, 0x86, 0xfa, 0x87, 0xdf, 0xae};
  static const uint8_t expected[ALK_STUN_HEADER_BYTES] = {
      0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xa4, 0x42, 0xb7, 0xe7,
      0xa7, 0x01, 0xbc, 0x34, 0xd6, 0x86, 0xfa, 0x87, 0xdf, 0xae,
  };
  uint8_t message[ALK_STUN_HEADER_BYTES];
  AlkStunMessageBuilder builder;
  CHECK(alk_stun_builder_init(&builder, message, sizeof(message), ALK_STUN_BINDING_REQUEST, transaction_id));
  CHECK(builder.length == sizeof(expected));
  CHECK(memcmp(message, expected, sizeof(expected)) == 0);
  CHECK(alk_stun_message_validate(message, builder.length));
  CHECK(alk_stun_message_matches(message, builder.length, ALK_STUN_BINDING_REQUEST, transaction_id));
  /* A request is not a success response, and another transaction does not match. */
  CHECK(!alk_stun_message_matches(message, builder.length, ALK_STUN_BINDING_SUCCESS_RESPONSE, transaction_id));
  static const uint8_t other_transaction[12] = {0};
  CHECK(!alk_stun_message_matches(message, builder.length, ALK_STUN_BINDING_REQUEST, other_transaction));
}

/* RFC 8489 section 5 header rules and section 14 attribute framing: the two
 * most significant bits are zero, the length counts whole 4-byte words and
 * exactly the bytes after the header, and the magic cookie is fixed. */
static void test_rfc8489_header_and_framing_rules(void) {
  static const uint8_t transaction_id[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
  uint8_t message[64];
  AlkStunMessageBuilder builder;
  CHECK(alk_stun_builder_init(&builder, message, sizeof(message), ALK_STUN_BINDING_REQUEST, transaction_id));
  CHECK(alk_stun_builder_add(&builder, 0x8022u, "abc", 3u));
  CHECK(builder.length == ALK_STUN_HEADER_BYTES + 8u);
  CHECK(alk_stun_message_validate(message, builder.length));

  uint8_t bad[64];
  memcpy(bad, message, builder.length);
  bad[0] |= 0x40u;
  CHECK(!alk_stun_message_validate(bad, builder.length));
  memcpy(bad, message, builder.length);
  bad[4] ^= 1u;
  CHECK(!alk_stun_message_validate(bad, builder.length));
  memcpy(bad, message, builder.length);
  bad[3] = 7u; /* not a multiple of four */
  CHECK(!alk_stun_message_validate(bad, builder.length));
  memcpy(bad, message, builder.length);
  bad[3] = 4u; /* shorter than the bytes that follow the header */
  CHECK(!alk_stun_message_validate(bad, builder.length));
  memcpy(bad, message, builder.length);
  bad[ALK_STUN_HEADER_BYTES + 3u] = 40u; /* attribute longer than the message */
  CHECK(!alk_stun_message_validate(bad, builder.length));
  CHECK(!alk_stun_message_validate(message, builder.length - 1u));
  CHECK(!alk_stun_message_validate(message, ALK_STUN_HEADER_BYTES - 1u));
}

/* RFC 5769 section 2.2 and 2.3 (the test vectors RFC 8489 appendix B extends):
 * a Binding success response carrying XOR-MAPPED-ADDRESS, as a probe receives it. */
static void test_rfc5769_binding_responses(void) {
  static const uint8_t ipv4_response[] = {
      0x01, 0x01, 0x00, 0x3c, 0x21, 0x12, 0xa4, 0x42, 0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34, 0xd6, 0x86,
      0xfa, 0x87, 0xdf, 0xae, 0x80, 0x22, 0x00, 0x0b, 0x74, 0x65, 0x73, 0x74, 0x20, 0x76, 0x65, 0x63,
      0x74, 0x6f, 0x72, 0x20, 0x00, 0x20, 0x00, 0x08, 0x00, 0x01, 0xa1, 0x47, 0xe1, 0x12, 0xa6, 0x43,
      0x00, 0x08, 0x00, 0x14, 0x2b, 0x91, 0xf5, 0x99, 0xfd, 0x9e, 0x90, 0xc3, 0x8c, 0x74, 0x89, 0xf9,
      0x2a, 0xf9, 0xba, 0x53, 0xf0, 0x6b, 0xe7, 0xd7, 0x80, 0x28, 0x00, 0x04, 0xc0, 0x7d, 0x4c, 0x96,
  };
  static const uint8_t ipv6_response[] = {
      0x01, 0x01, 0x00, 0x48, 0x21, 0x12, 0xa4, 0x42, 0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34, 0xd6, 0x86, 0xfa, 0x87, 0xdf,
      0xae, 0x80, 0x22, 0x00, 0x0b, 0x74, 0x65, 0x73, 0x74, 0x20, 0x76, 0x65, 0x63, 0x74, 0x6f, 0x72, 0x20, 0x00, 0x20,
      0x00, 0x14, 0x00, 0x02, 0xa1, 0x47, 0x01, 0x13, 0xa9, 0xfa, 0xa5, 0xd3, 0xf1, 0x79, 0xbc, 0x25, 0xf4, 0xb5, 0xbe,
      0xd2, 0xb9, 0xd9, 0x00, 0x08, 0x00, 0x14, 0xa3, 0x82, 0x95, 0x4e, 0x4b, 0xe6, 0x7b, 0xf1, 0x17, 0x84, 0xc9, 0x7c,
      0x82, 0x92, 0xc2, 0x75, 0xbf, 0xe3, 0xed, 0x41, 0x80, 0x28, 0x00, 0x04, 0xc8, 0xfb, 0x0b, 0x4c,
  };
  static const uint8_t transaction_id[12] = {0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34, 0xd6, 0x86, 0xfa, 0x87, 0xdf, 0xae};
  static const char password[] = "VOkJxbRl1RmTxUk/WvJxBt";
  AlkStunMappedEndpoint mapped;

  CHECK(alk_stun_message_validate(ipv4_response, sizeof(ipv4_response)));
  CHECK(alk_stun_message_matches(ipv4_response, sizeof(ipv4_response), ALK_STUN_BINDING_SUCCESS_RESPONSE,
                                 transaction_id));
  CHECK(alk_stun_decode_mapped_endpoint(ipv4_response, sizeof(ipv4_response), transaction_id, &mapped));
  CHECK(mapped.address_family == ALK_STUN_ADDRESS_IPV4);
  CHECK(mapped.port == 32853u);
  CHECK(mapped.address[0] == 192u && mapped.address[1] == 0u && mapped.address[2] == 2u && mapped.address[3] == 1u);
  CHECK(alk_stun_verify_integrity(ipv4_response, sizeof(ipv4_response), ALK_STUN_INTEGRITY_SHA1, password,
                                  sizeof(password) - 1u));
  CHECK(alk_stun_verify_fingerprint(ipv4_response, sizeof(ipv4_response)));

  CHECK(alk_stun_message_validate(ipv6_response, sizeof(ipv6_response)));
  CHECK(alk_stun_decode_mapped_endpoint(ipv6_response, sizeof(ipv6_response), transaction_id, &mapped));
  static const uint8_t expected_v6[16] = {0x20, 0x01, 0x0d, 0xb8, 0x12, 0x34, 0x56, 0x78,
                                          0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
  CHECK(mapped.address_family == ALK_STUN_ADDRESS_IPV6);
  CHECK(mapped.port == 32853u);
  CHECK(memcmp(mapped.address, expected_v6, sizeof(expected_v6)) == 0);
  CHECK(alk_stun_verify_integrity(ipv6_response, sizeof(ipv6_response), ALK_STUN_INTEGRITY_SHA1, password,
                                  sizeof(password) - 1u));
  CHECK(alk_stun_verify_fingerprint(ipv6_response, sizeof(ipv6_response)));

  /* The response is bound to its transaction: another ID decodes nothing. */
  static const uint8_t other_transaction[12] = {9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9};
  CHECK(!alk_stun_decode_mapped_endpoint(ipv4_response, sizeof(ipv4_response), other_transaction, &mapped));

  /* Encoding the same facts reproduces the vector's attribute bytes (RFC 8489
   * section 14.2: port XOR the cookie's high half, address XOR cookie then ID). */
  uint8_t rebuilt[128];
  AlkStunMessageBuilder builder;
  AlkStunMappedEndpoint endpoint;
  AlkStunAttributeView expected_attribute;
  AlkStunAttributeView rebuilt_attribute;
  alk_stun_mapped_endpoint_init(&endpoint);
  endpoint.address_family = ALK_STUN_ADDRESS_IPV4;
  endpoint.port = 32853u;
  endpoint.address[0] = 192u;
  endpoint.address[1] = 0u;
  endpoint.address[2] = 2u;
  endpoint.address[3] = 1u;
  CHECK(alk_stun_builder_init(&builder, rebuilt, sizeof(rebuilt), ALK_STUN_BINDING_SUCCESS_RESPONSE, transaction_id));
  CHECK(alk_stun_builder_add_xor_mapped_endpoint(&builder, &endpoint));
  CHECK(alk_stun_find_attribute(ipv4_response, sizeof(ipv4_response), ALK_STUN_ATTR_XOR_MAPPED_ADDRESS,
                                &expected_attribute));
  CHECK(alk_stun_find_attribute(rebuilt, builder.length, ALK_STUN_ATTR_XOR_MAPPED_ADDRESS, &rebuilt_attribute));
  CHECK(rebuilt_attribute.length == expected_attribute.length &&
        memcmp(rebuilt_attribute.value, expected_attribute.value, expected_attribute.length) == 0);

  alk_stun_mapped_endpoint_init(&endpoint);
  endpoint.address_family = ALK_STUN_ADDRESS_IPV6;
  endpoint.port = 32853u;
  memcpy(endpoint.address, expected_v6, sizeof(expected_v6));
  CHECK(alk_stun_builder_init(&builder, rebuilt, sizeof(rebuilt), ALK_STUN_BINDING_SUCCESS_RESPONSE, transaction_id));
  CHECK(alk_stun_builder_add_xor_mapped_endpoint(&builder, &endpoint));
  CHECK(alk_stun_find_attribute(ipv6_response, sizeof(ipv6_response), ALK_STUN_ATTR_XOR_MAPPED_ADDRESS,
                                &expected_attribute));
  CHECK(alk_stun_find_attribute(rebuilt, builder.length, ALK_STUN_ATTR_XOR_MAPPED_ADDRESS, &rebuilt_attribute));
  CHECK(rebuilt_attribute.length == expected_attribute.length &&
        memcmp(rebuilt_attribute.value, expected_attribute.value, expected_attribute.length) == 0);
}

/* RFC 8489 section 14.1 and 14.2 with section 12: XOR-MAPPED-ADDRESS is
 * preferred, and a plain MAPPED-ADDRESS from an older server still decodes. */
static void test_rfc8489_mapped_address_preference(void) {
  static const uint8_t transaction_id[12] = {4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 6, 6};
  static const uint8_t plain[8] = {0x00, 0x01, 0x1f, 0x90, 198, 51, 100, 7};
  uint8_t message[96];
  AlkStunMessageBuilder builder;
  AlkStunMappedEndpoint mapped;

  CHECK(alk_stun_builder_init(&builder, message, sizeof(message), ALK_STUN_BINDING_SUCCESS_RESPONSE, transaction_id));
  CHECK(alk_stun_builder_add(&builder, ALK_STUN_ATTR_MAPPED_ADDRESS, plain, sizeof(plain)));
  CHECK(alk_stun_decode_mapped_endpoint(message, builder.length, transaction_id, &mapped));
  CHECK(mapped.port == 8080u && mapped.address[0] == 198u && mapped.address[3] == 7u);

  AlkStunMappedEndpoint xor_endpoint;
  alk_stun_mapped_endpoint_init(&xor_endpoint);
  xor_endpoint.address_family = ALK_STUN_ADDRESS_IPV4;
  xor_endpoint.port = 49152u;
  xor_endpoint.address[0] = 203u;
  xor_endpoint.address[1] = 0u;
  xor_endpoint.address[2] = 113u;
  xor_endpoint.address[3] = 9u;
  CHECK(alk_stun_builder_add_xor_mapped_endpoint(&builder, &xor_endpoint));
  CHECK(alk_stun_decode_mapped_endpoint(message, builder.length, transaction_id, &mapped));
  CHECK(mapped.port == 49152u && mapped.address[0] == 203u);

  /* An error response carries no mapped address. */
  CHECK(alk_stun_builder_init(&builder, message, sizeof(message), 0x0111u, transaction_id));
  CHECK(alk_stun_builder_add_xor_mapped_endpoint(&builder, &xor_endpoint));
  CHECK(!alk_stun_decode_mapped_endpoint(message, builder.length, transaction_id, &mapped));
}

/* RFC 8489 appendix B.1: a long-term-credential request with USERHASH and
 * MESSAGE-INTEGRITY-SHA256 (key = SHA-256 of "username:realm:password", section
 * 9.2.2). As printed, the vector's header length is 0x9c, which is the whole
 * message size and not the attribute length 0x88 (136); its HMAC covers that
 * header, so a decoder that checks the length rightly rejects it. The same
 * message with the length corrected and the HMAC recomputed over the corrected
 * header (done independently with a second implementation) is accepted. */
static void test_rfc8489_appendix_b1_long_term_sha256(void) {
  static const uint8_t printed_request[] = {
      0x00, 0x01, 0x00, 0x9c, 0x21, 0x12, 0xa4, 0x42, 0x78, 0xad, 0x34, 0x33, 0xc6, 0xad, 0x72, 0xc0, 0x29, 0xda,
      0x41, 0x2e, 0x00, 0x1e, 0x00, 0x20, 0x4a, 0x3c, 0xf3, 0x8f, 0xef, 0x69, 0x92, 0xbd, 0xa9, 0x52, 0xc6, 0x78,
      0x04, 0x17, 0xda, 0x0f, 0x24, 0x81, 0x94, 0x15, 0x56, 0x9e, 0x60, 0xb2, 0x05, 0xc4, 0x6e, 0x41, 0x40, 0x7f,
      0x17, 0x04, 0x00, 0x15, 0x00, 0x29, 0x6f, 0x62, 0x4d, 0x61, 0x74, 0x4a, 0x6f, 0x73, 0x32, 0x41, 0x41, 0x41,
      0x43, 0x66, 0x2f, 0x2f, 0x34, 0x39, 0x39, 0x6b, 0x39, 0x35, 0x34, 0x64, 0x36, 0x4f, 0x4c, 0x33, 0x34, 0x6f,
      0x4c, 0x39, 0x46, 0x53, 0x54, 0x76, 0x79, 0x36, 0x34, 0x73, 0x41, 0x00, 0x00, 0x00, 0x00, 0x14, 0x00, 0x0b,
      0x65, 0x78, 0x61, 0x6d, 0x70, 0x6c, 0x65, 0x2e, 0x6f, 0x72, 0x67, 0x00, 0x00, 0x1c, 0x00, 0x20, 0xe4, 0x68,
      0x6c, 0x8f, 0x0e, 0xde, 0xb5, 0x90, 0x13, 0xe0, 0x70, 0x90, 0x01, 0x0a, 0x93, 0xef, 0xcc, 0xbc, 0xcc, 0x54,
      0x4c, 0x0a, 0x45, 0xd9, 0xf8, 0x30, 0xaa, 0x6d, 0x6f, 0x73, 0x5a, 0x01,
  };
  static const uint8_t corrected_integrity[32] = {
      0x23, 0x41, 0x12, 0xfb, 0xd4, 0xe2, 0x7f, 0x98, 0x3e, 0xb4, 0x03, 0x28, 0x36, 0xf9, 0x98, 0x21,
      0x6f, 0x5b, 0x23, 0xf8, 0xd9, 0x27, 0x75, 0x3f, 0xbc, 0x4f, 0x88, 0x2b, 0xfb, 0xdf, 0x0d, 0xec,
  };
  static const uint8_t transaction_id[12] = {0x78, 0xad, 0x34, 0x33, 0xc6, 0xad, 0x72, 0xc0, 0x29, 0xda, 0x41, 0x2e};
  static const char username[] = "\xe3\x83\x9e\xe3\x83\x88\xe3\x83\xaa\xe3\x83"
                                 "\x83\xe3\x82\xaf\xe3\x82\xb9";
  static const char realm[] = "example.org";
  static const char password[] = "TheMatrIX";
  uint8_t key[32];
  size_t key_length = 0u;
  CHECK(alk_stun_derive_long_term_key(ALK_STUN_PASSWORD_SHA256, username, sizeof(username) - 1u, realm,
                                      sizeof(realm) - 1u, password, sizeof(password) - 1u, key, sizeof(key),
                                      &key_length));
  CHECK(key_length == 32u);

  CHECK(sizeof(printed_request) - ALK_STUN_HEADER_BYTES == 136u);
  CHECK(printed_request[2] == 0x00u && printed_request[3] == 0x9cu);
  CHECK(!alk_stun_message_validate(printed_request, sizeof(printed_request)));

  uint8_t corrected[sizeof(printed_request)];
  memcpy(corrected, printed_request, sizeof(corrected));
  corrected[3] = 0x88u;
  memcpy(corrected + sizeof(corrected) - 32u, corrected_integrity, 32u);
  CHECK(alk_stun_message_validate(corrected, sizeof(corrected)));
  CHECK(alk_stun_message_matches(corrected, sizeof(corrected), ALK_STUN_BINDING_REQUEST, transaction_id));
  CHECK(alk_stun_verify_integrity(corrected, sizeof(corrected), ALK_STUN_INTEGRITY_SHA256, key, key_length));
  /* The SHA-1 attribute type is not there, and a wrong key fails. */
  CHECK(!alk_stun_verify_integrity(corrected, sizeof(corrected), ALK_STUN_INTEGRITY_SHA1, key, key_length));
  key[0] ^= 1u;
  CHECK(!alk_stun_verify_integrity(corrected, sizeof(corrected), ALK_STUN_INTEGRITY_SHA256, key, key_length));

  /* USERHASH (0x001e) is SHA-256("username:realm"), 32 bytes. */
  AlkStunAttributeView userhash;
  CHECK(alk_stun_find_attribute(corrected, sizeof(corrected), 0x001eu, &userhash));
  CHECK(userhash.length == 32u);
  uint8_t input[64];
  size_t input_length = 0u;
  memcpy(input, username, sizeof(username) - 1u);
  input_length += sizeof(username) - 1u;
  input[input_length++] = ':';
  memcpy(input + input_length, realm, sizeof(realm) - 1u);
  input_length += sizeof(realm) - 1u;
  uint8_t digest[SHA256_DIGEST_LENGTH];
  SHA256(input, input_length, digest);
  CHECK(memcmp(userhash.value, digest, sizeof(digest)) == 0);
}

int main(void) {
  test_xor_mapped_ipv4();
  test_xor_mapped_ipv6();
  test_rejects_wrong_transaction();
  test_integrity_and_fingerprint(ALK_STUN_INTEGRITY_SHA1);
  test_integrity_and_fingerprint(ALK_STUN_INTEGRITY_SHA256);
  test_rfc5769_short_term_request_vector();
  test_xor_mapped_roundtrip();
  test_rfc5780_diagnostic_attributes();
  test_long_term_key_derivation();
  test_turn_extension_primitives();
  test_rfc8489_binding_request_encoding();
  test_rfc8489_header_and_framing_rules();
  test_rfc5769_binding_responses();
  test_rfc8489_mapped_address_preference();
  test_rfc8489_appendix_b1_long_term_sha256();
  if (failures != 0) {
    fprintf(stderr, "stun_codec_test failures=%d\n", failures);
    return 1;
  }
  puts("stun_codec_test passed");
  return 0;
}
