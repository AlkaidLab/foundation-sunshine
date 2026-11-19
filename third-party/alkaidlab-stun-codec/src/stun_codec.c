#include "alkaidlab/network/stun_codec.h"

#include <limits.h>
#include <string.h>

#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/params.h>

static uint16_t read_be16(const uint8_t *p) {
  return (uint16_t)(((uint16_t)p[0] << 8u) | p[1]);
}

static uint32_t read_be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24u) | ((uint32_t)p[1] << 16u) |
         ((uint32_t)p[2] << 8u) | (uint32_t)p[3];
}

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

static void write_be64(uint8_t *p, uint64_t value) {
  write_be32(p, (uint32_t)(value >> 32u));
  write_be32(p + 4u, (uint32_t)value);
}

static bool message_bounds(const uint8_t *message, size_t message_length,
                           size_t *out_end) {
  if (message == NULL || message_length < ALK_STUN_HEADER_BYTES ||
      (message[0] & 0xc0u) != 0u ||
      read_be32(message + 4u) != ALK_STUN_MAGIC_COOKIE) {
    return false;
  }
  const uint16_t payload_length = read_be16(message + 2u);
  if ((payload_length & 3u) != 0u ||
      (size_t)payload_length != message_length - ALK_STUN_HEADER_BYTES) {
    return false;
  }
  if (out_end != NULL)
    *out_end = message_length;
  return true;
}

bool alk_stun_message_validate(const uint8_t *message, size_t message_length) {
  size_t end = 0u;
  if (!message_bounds(message, message_length, &end))
    return false;
  size_t offset = ALK_STUN_HEADER_BYTES;
  while (offset < end) {
    if (offset + 4u > end)
      return false;
    const uint16_t length = read_be16(message + offset + 2u);
    const size_t padded = ((size_t)length + 3u) & ~(size_t)3u;
    if (offset + 4u + padded > end)
      return false;
    offset += 4u + padded;
  }
  return offset == end;
}

bool alk_stun_find_attribute(const uint8_t *message, size_t message_length,
                             uint16_t type, AlkStunAttributeView *attribute) {
  if (attribute == NULL ||
      !alk_stun_message_validate(message, message_length)) {
    return false;
  }
  for (size_t offset = ALK_STUN_HEADER_BYTES; offset < message_length;) {
    const uint16_t current_type = read_be16(message + offset);
    const uint16_t current_length = read_be16(message + offset + 2u);
    if (current_type == type) {
      attribute->type = current_type;
      attribute->length = current_length;
      attribute->offset = offset;
      attribute->value = message + offset + 4u;
      return true;
    }
    offset += 4u + (((size_t)current_length + 3u) & ~(size_t)3u);
  }
  return false;
}

static bool update_message_length(AlkStunMessageBuilder *builder) {
  if (builder == NULL || builder->bytes == NULL ||
      builder->length < ALK_STUN_HEADER_BYTES ||
      builder->length - ALK_STUN_HEADER_BYTES > UINT16_MAX) {
    return false;
  }
  write_be16(builder->bytes + 2u,
             (uint16_t)(builder->length - ALK_STUN_HEADER_BYTES));
  return true;
}

bool alk_stun_builder_init(AlkStunMessageBuilder *builder, uint8_t *storage,
                           size_t capacity, uint16_t message_type,
                           const uint8_t transaction_id[12]) {
  if (builder == NULL || storage == NULL || transaction_id == NULL ||
      capacity < ALK_STUN_HEADER_BYTES || (message_type & 0xc000u) != 0u) {
    return false;
  }
  memset(builder, 0, sizeof(*builder));
  memset(storage, 0, ALK_STUN_HEADER_BYTES);
  builder->bytes = storage;
  builder->capacity = capacity;
  builder->length = ALK_STUN_HEADER_BYTES;
  write_be16(storage, message_type);
  write_be32(storage + 4u, ALK_STUN_MAGIC_COOKIE);
  memcpy(storage + 8u, transaction_id, 12u);
  return true;
}

bool alk_stun_builder_add(AlkStunMessageBuilder *builder, uint16_t type,
                          const void *value, uint16_t value_length) {
  if (builder == NULL || builder->bytes == NULL || builder->integrity_added ||
      builder->fingerprint_added || (value == NULL && value_length != 0u)) {
    return false;
  }
  const size_t padded = ((size_t)value_length + 3u) & ~(size_t)3u;
  if (builder->length > builder->capacity ||
      4u + padded > builder->capacity - builder->length) {
    return false;
  }
  uint8_t *attribute = builder->bytes + builder->length;
  write_be16(attribute, type);
  write_be16(attribute + 2u, value_length);
  if (value_length != 0u)
    memcpy(attribute + 4u, value, value_length);
  if (padded > value_length) {
    memset(attribute + 4u + value_length, 0, padded - value_length);
  }
  builder->length += 4u + padded;
  return update_message_length(builder);
}

bool alk_stun_builder_add_u32(AlkStunMessageBuilder *builder, uint16_t type,
                              uint32_t value) {
  uint8_t encoded[4];
  write_be32(encoded, value);
  return alk_stun_builder_add(builder, type, encoded, sizeof(encoded));
}

bool alk_stun_builder_add_u64(AlkStunMessageBuilder *builder, uint16_t type,
                              uint64_t value) {
  uint8_t encoded[8];
  write_be64(encoded, value);
  return alk_stun_builder_add(builder, type, encoded, sizeof(encoded));
}

bool alk_stun_builder_add_flag(AlkStunMessageBuilder *builder, uint16_t type) {
  return alk_stun_builder_add(builder, type, NULL, 0u);
}

bool alk_stun_builder_add_change_request(AlkStunMessageBuilder *builder,
                                         bool change_ip, bool change_port) {
  uint32_t flags = 0u;
  if (change_ip)
    flags |= 0x04u;
  if (change_port)
    flags |= 0x02u;
  return alk_stun_builder_add_u32(builder, ALK_STUN_ATTR_CHANGE_REQUEST, flags);
}

bool alk_stun_builder_add_xor_endpoint(AlkStunMessageBuilder *builder,
                                       uint16_t attribute_type,
                                       const AlkStunMappedEndpoint *endpoint) {
  if (builder == NULL || endpoint == NULL || endpoint->port == 0u)
    return false;
  const uint8_t *transaction_id = builder->bytes + 8u;
  uint8_t encoded[20] = {0};
  size_t address_length = 0u;
  if (endpoint->address_family == ALK_STUN_ADDRESS_IPV4) {
    encoded[1] = 0x01u;
    address_length = 4u;
  } else if (endpoint->address_family == ALK_STUN_ADDRESS_IPV6) {
    encoded[1] = 0x02u;
    address_length = 16u;
  } else {
    return false;
  }
  write_be16(encoded + 2u,
             endpoint->port ^ (uint16_t)(ALK_STUN_MAGIC_COOKIE >> 16u));
  const uint8_t mask[16] = {
      0x21u,
      0x12u,
      0xa4u,
      0x42u,
      transaction_id[0],
      transaction_id[1],
      transaction_id[2],
      transaction_id[3],
      transaction_id[4],
      transaction_id[5],
      transaction_id[6],
      transaction_id[7],
      transaction_id[8],
      transaction_id[9],
      transaction_id[10],
      transaction_id[11],
  };
  for (size_t index = 0u; index < address_length; ++index) {
    encoded[4u + index] = endpoint->address[index] ^ mask[index];
  }
  return alk_stun_builder_add(builder, attribute_type, encoded,
                              (uint16_t)(4u + address_length));
}

bool alk_stun_builder_add_xor_mapped_endpoint(
    AlkStunMessageBuilder *builder, const AlkStunMappedEndpoint *endpoint) {
  return alk_stun_builder_add_xor_endpoint(
      builder, ALK_STUN_ATTR_XOR_MAPPED_ADDRESS, endpoint);
}

static bool hmac_message_prefix(const char *digest_name, const void *key,
                                size_t key_length, const uint8_t *message,
                                size_t attribute_offset,
                                uint16_t adjusted_payload_length,
                                uint8_t *output, size_t output_capacity,
                                size_t *output_length) {
  if (digest_name == NULL || key == NULL || key_length == 0u ||
      message == NULL || attribute_offset < ALK_STUN_HEADER_BYTES ||
      output == NULL || output_length == NULL) {
    return false;
  }
  EVP_MAC *mac = EVP_MAC_fetch(NULL, "HMAC", NULL);
  EVP_MAC_CTX *context = mac != NULL ? EVP_MAC_CTX_new(mac) : NULL;
  char *digest = (char *)digest_name;
  OSSL_PARAM parameters[] = {
      OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, digest, 0u),
      OSSL_PARAM_construct_end(),
  };
  const uint8_t adjusted_length[2] = {
      (uint8_t)(adjusted_payload_length >> 8u),
      (uint8_t)adjusted_payload_length,
  };
  bool ok =
      context != NULL &&
      EVP_MAC_init(context, (const unsigned char *)key, key_length,
                   parameters) == 1 &&
      EVP_MAC_update(context, message, 2u) == 1 &&
      EVP_MAC_update(context, adjusted_length, sizeof(adjusted_length)) == 1 &&
      EVP_MAC_update(context, message + 4u, attribute_offset - 4u) == 1 &&
      EVP_MAC_final(context, output, output_length, output_capacity) == 1;
  EVP_MAC_CTX_free(context);
  EVP_MAC_free(mac);
  return ok;
}

static bool integrity_shape(uint32_t algorithm, uint16_t *attribute_type,
                            uint16_t *tag_length, const char **digest_name) {
  if (attribute_type == NULL || tag_length == NULL || digest_name == NULL) {
    return false;
  }
  if (algorithm == ALK_STUN_INTEGRITY_SHA1) {
    *attribute_type = ALK_STUN_ATTR_MESSAGE_INTEGRITY;
    *tag_length = 20u;
    *digest_name = "SHA1";
    return true;
  }
  if (algorithm == ALK_STUN_INTEGRITY_SHA256) {
    *attribute_type = ALK_STUN_ATTR_MESSAGE_INTEGRITY_SHA256;
    *tag_length = 32u;
    *digest_name = "SHA256";
    return true;
  }
  return false;
}

bool alk_stun_builder_add_integrity(AlkStunMessageBuilder *builder,
                                    uint32_t algorithm, const void *key,
                                    size_t key_length) {
  uint16_t type = 0u;
  uint16_t tag_length = 0u;
  const char *digest_name = NULL;
  if (builder == NULL || builder->bytes == NULL || builder->integrity_added ||
      builder->fingerprint_added || key == NULL || key_length == 0u ||
      !integrity_shape(algorithm, &type, &tag_length, &digest_name)) {
    return false;
  }
  const size_t attribute_offset = builder->length;
  const size_t final_length = attribute_offset + 4u + tag_length;
  if (final_length > builder->capacity ||
      final_length - ALK_STUN_HEADER_BYTES > UINT16_MAX) {
    return false;
  }
  write_be16(builder->bytes + attribute_offset, type);
  write_be16(builder->bytes + attribute_offset + 2u, tag_length);
  memset(builder->bytes + attribute_offset + 4u, 0, tag_length);
  builder->length = final_length;
  if (!update_message_length(builder))
    return false;
  uint8_t tag[32];
  size_t actual_length = 0u;
  if (!hmac_message_prefix(digest_name, key, key_length, builder->bytes,
                           attribute_offset,
                           (uint16_t)(final_length - ALK_STUN_HEADER_BYTES),
                           tag, sizeof(tag), &actual_length) ||
      actual_length != tag_length) {
    builder->length = attribute_offset;
    (void)update_message_length(builder);
    return false;
  }
  memcpy(builder->bytes + attribute_offset + 4u, tag, tag_length);
  builder->integrity_added = true;
  return true;
}

static uint32_t crc32(const uint8_t *data, size_t length) {
  uint32_t crc = 0xffffffffu;
  for (size_t index = 0u; index < length; ++index) {
    crc ^= data[index];
    for (unsigned bit = 0u; bit < 8u; ++bit) {
      const uint32_t mask = 0u - (crc & 1u);
      crc = (crc >> 1u) ^ (0xedb88320u & mask);
    }
  }
  return ~crc;
}

bool alk_stun_builder_add_fingerprint(AlkStunMessageBuilder *builder) {
  if (builder == NULL || builder->bytes == NULL || builder->fingerprint_added ||
      builder->length > builder->capacity ||
      8u > builder->capacity - builder->length) {
    return false;
  }
  const size_t attribute_offset = builder->length;
  write_be16(builder->bytes + attribute_offset, ALK_STUN_ATTR_FINGERPRINT);
  write_be16(builder->bytes + attribute_offset + 2u, 4u);
  write_be32(builder->bytes + attribute_offset + 4u, 0u);
  builder->length += 8u;
  if (!update_message_length(builder))
    return false;
  write_be32(builder->bytes + attribute_offset + 4u,
             crc32(builder->bytes, attribute_offset) ^ 0x5354554eu);
  builder->fingerprint_added = true;
  return true;
}

bool alk_stun_verify_integrity(const uint8_t *message, size_t message_length,
                               uint32_t algorithm, const void *key,
                               size_t key_length) {
  uint16_t type = 0u;
  uint16_t tag_length = 0u;
  const char *digest_name = NULL;
  if (key == NULL || key_length == 0u ||
      !integrity_shape(algorithm, &type, &tag_length, &digest_name)) {
    return false;
  }
  AlkStunAttributeView integrity;
  if (!alk_stun_find_attribute(message, message_length, type, &integrity) ||
      integrity.length != tag_length ||
      integrity.offset + 4u + integrity.length > message_length) {
    return false;
  }
  const size_t integrity_end = integrity.offset + 4u + integrity.length;
  if (integrity_end - ALK_STUN_HEADER_BYTES > UINT16_MAX)
    return false;
  uint8_t expected[32];
  size_t expected_length = 0u;
  if (!hmac_message_prefix(digest_name, key, key_length, message,
                           integrity.offset,
                           (uint16_t)(integrity_end - ALK_STUN_HEADER_BYTES),
                           expected, sizeof(expected), &expected_length) ||
      expected_length != integrity.length) {
    return false;
  }
  return CRYPTO_memcmp(expected, integrity.value, integrity.length) == 0;
}

bool alk_stun_derive_long_term_key(uint32_t password_algorithm,
                                   const void *username, size_t username_length,
                                   const void *realm, size_t realm_length,
                                   const void *password, size_t password_length,
                                   uint8_t *key, size_t key_capacity,
                                   size_t *key_length) {
  if (key_length != NULL)
    *key_length = 0u;
  if (username == NULL || username_length == 0u || realm == NULL ||
      realm_length == 0u || password == NULL || password_length == 0u ||
      key == NULL || key_length == NULL) {
    return false;
  }
  const char *digest_name = NULL;
  size_t required = 0u;
  if (password_algorithm == ALK_STUN_PASSWORD_MD5) {
    digest_name = "MD5";
    required = 16u;
  } else if (password_algorithm == ALK_STUN_PASSWORD_SHA256) {
    digest_name = "SHA256";
    required = 32u;
  } else {
    return false;
  }
  if (key_capacity < required)
    return false;

  EVP_MD *digest = EVP_MD_fetch(NULL, digest_name, NULL);
  EVP_MD_CTX *context = digest != NULL ? EVP_MD_CTX_new() : NULL;
  unsigned int actual = 0u;
  static const uint8_t colon = ':';
  const bool ok =
      context != NULL && EVP_DigestInit_ex(context, digest, NULL) == 1 &&
      EVP_DigestUpdate(context, username, username_length) == 1 &&
      EVP_DigestUpdate(context, &colon, 1u) == 1 &&
      EVP_DigestUpdate(context, realm, realm_length) == 1 &&
      EVP_DigestUpdate(context, &colon, 1u) == 1 &&
      EVP_DigestUpdate(context, password, password_length) == 1 &&
      EVP_DigestFinal_ex(context, key, &actual) == 1 && actual == required;
  EVP_MD_CTX_free(context);
  EVP_MD_free(digest);
  if (!ok) {
    OPENSSL_cleanse(key, required);
    return false;
  }
  *key_length = actual;
  return true;
}

bool alk_stun_verify_fingerprint(const uint8_t *message,
                                 size_t message_length) {
  AlkStunAttributeView fingerprint;
  if (!alk_stun_find_attribute(message, message_length,
                               ALK_STUN_ATTR_FINGERPRINT, &fingerprint) ||
      fingerprint.length != 4u || fingerprint.offset + 8u != message_length) {
    return false;
  }
  const uint32_t expected = crc32(message, fingerprint.offset) ^ 0x5354554eu;
  return read_be32(fingerprint.value) == expected;
}

bool alk_stun_message_matches(const uint8_t *message, size_t message_length,
                              uint16_t message_type,
                              const uint8_t transaction_id[12]) {
  return transaction_id != NULL &&
         alk_stun_message_validate(message, message_length) &&
         read_be16(message) == message_type &&
         memcmp(message + 8u, transaction_id, 12u) == 0;
}

bool alk_stun_decode_error_code(const uint8_t *message, size_t message_length,
                                const uint8_t transaction_id[12],
                                uint16_t *error_code) {
  if (error_code == NULL || transaction_id == NULL ||
      !alk_stun_message_validate(message, message_length) ||
      memcmp(message + 8u, transaction_id, 12u) != 0) {
    return false;
  }
  AlkStunAttributeView attribute;
  if (!alk_stun_find_attribute(message, message_length,
                               ALK_STUN_ATTR_ERROR_CODE, &attribute) ||
      attribute.length < 4u || attribute.value[0] != 0u ||
      attribute.value[1] != 0u || (attribute.value[2] & 0xf8u) != 0u) {
    return false;
  }
  const uint16_t value =
      (uint16_t)((uint16_t)attribute.value[2] * 100u + attribute.value[3]);
  if (value < 300u || value > 699u)
    return false;
  *error_code = value;
  return true;
}

void alk_stun_mapped_endpoint_init(AlkStunMappedEndpoint *endpoint) {
  if (endpoint == NULL)
    return;
  memset(endpoint, 0, sizeof(*endpoint));
  endpoint->version = ALK_STUN_MAPPED_ENDPOINT_VERSION;
}

static bool decode_address_attribute(const uint8_t *value, size_t value_length,
                                     uint16_t attribute_type,
                                     const uint8_t transaction_id[12],
                                     AlkStunMappedEndpoint *endpoint) {
  if (value == NULL || transaction_id == NULL || endpoint == NULL ||
      value_length < 8u || value[0] != 0u) {
    return false;
  }
  size_t address_length = 0u;
  uint32_t address_family = ALK_STUN_ADDRESS_UNKNOWN;
  if (value[1] == 0x01u && value_length >= 8u) {
    address_length = 4u;
    address_family = ALK_STUN_ADDRESS_IPV4;
  } else if (value[1] == 0x02u && value_length >= 20u) {
    address_length = 16u;
    address_family = ALK_STUN_ADDRESS_IPV6;
  } else {
    return false;
  }
  alk_stun_mapped_endpoint_init(endpoint);
  endpoint->address_family = address_family;
  endpoint->port = read_be16(value + 2u);
  if (attribute_type == ALK_STUN_ATTR_XOR_MAPPED_ADDRESS) {
    endpoint->port ^= (uint16_t)(ALK_STUN_MAGIC_COOKIE >> 16u);
    const uint8_t mask[16] = {
        0x21u,
        0x12u,
        0xa4u,
        0x42u,
        transaction_id[0],
        transaction_id[1],
        transaction_id[2],
        transaction_id[3],
        transaction_id[4],
        transaction_id[5],
        transaction_id[6],
        transaction_id[7],
        transaction_id[8],
        transaction_id[9],
        transaction_id[10],
        transaction_id[11],
    };
    for (size_t index = 0u; index < address_length; ++index) {
      endpoint->address[index] = value[4u + index] ^ mask[index];
    }
  } else {
    memcpy(endpoint->address, value + 4u, address_length);
  }
  return endpoint->port != 0u;
}

bool alk_stun_decode_xor_endpoint(const uint8_t *message, size_t message_length,
                                  const uint8_t transaction_id[12],
                                  uint16_t attribute_type,
                                  AlkStunMappedEndpoint *endpoint) {
  if (transaction_id == NULL || endpoint == NULL ||
      !alk_stun_message_validate(message, message_length) ||
      memcmp(message + 8u, transaction_id, 12u) != 0) {
    return false;
  }
  AlkStunAttributeView attribute;
  return alk_stun_find_attribute(message, message_length, attribute_type,
                                 &attribute) &&
         decode_address_attribute(attribute.value, attribute.length,
                                  ALK_STUN_ATTR_XOR_MAPPED_ADDRESS,
                                  transaction_id, endpoint);
}

bool alk_stun_decode_mapped_endpoint(const uint8_t *response,
                                     size_t response_length,
                                     const uint8_t transaction_id[12],
                                     AlkStunMappedEndpoint *endpoint) {
  if (transaction_id == NULL || endpoint == NULL ||
      !alk_stun_message_validate(response, response_length) ||
      read_be16(response) != ALK_STUN_BINDING_SUCCESS_RESPONSE ||
      memcmp(response + 8u, transaction_id, 12u) != 0) {
    return false;
  }
  AlkStunAttributeView attribute;
  if (alk_stun_find_attribute(response, response_length,
                              ALK_STUN_ATTR_XOR_MAPPED_ADDRESS, &attribute) &&
      decode_address_attribute(attribute.value, attribute.length,
                               attribute.type, transaction_id, endpoint)) {
    return true;
  }
  return alk_stun_find_attribute(response, response_length,
                                 ALK_STUN_ATTR_MAPPED_ADDRESS, &attribute) &&
         decode_address_attribute(attribute.value, attribute.length,
                                  attribute.type, transaction_id, endpoint);
}

static bool decode_response_address(const uint8_t *response,
                                    size_t response_length,
                                    const uint8_t transaction_id[12],
                                    uint16_t attribute_type,
                                    AlkStunMappedEndpoint *endpoint) {
  if (transaction_id == NULL || endpoint == NULL ||
      !alk_stun_message_validate(response, response_length) ||
      read_be16(response) != ALK_STUN_BINDING_SUCCESS_RESPONSE ||
      memcmp(response + 8u, transaction_id, 12u) != 0) {
    return false;
  }
  AlkStunAttributeView attribute;
  return alk_stun_find_attribute(response, response_length, attribute_type,
                                 &attribute) &&
         decode_address_attribute(attribute.value, attribute.length,
                                  attribute.type, transaction_id, endpoint);
}

bool alk_stun_decode_response_origin(const uint8_t *response,
                                     size_t response_length,
                                     const uint8_t transaction_id[12],
                                     AlkStunMappedEndpoint *endpoint) {
  return decode_response_address(response, response_length, transaction_id,
                                 ALK_STUN_ATTR_RESPONSE_ORIGIN, endpoint);
}

bool alk_stun_decode_other_address(const uint8_t *response,
                                   size_t response_length,
                                   const uint8_t transaction_id[12],
                                   AlkStunMappedEndpoint *endpoint) {
  return decode_response_address(response, response_length, transaction_id,
                                 ALK_STUN_ATTR_OTHER_ADDRESS, endpoint);
}
