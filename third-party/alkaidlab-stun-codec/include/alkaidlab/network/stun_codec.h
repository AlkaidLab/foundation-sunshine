#ifndef ALKAIDLAB_NETWORK_STUN_CODEC_H
#define ALKAIDLAB_NETWORK_STUN_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ALK_STUN_MAPPED_ENDPOINT_VERSION 1u
#define ALK_STUN_HEADER_BYTES 20u
#define ALK_STUN_MAGIC_COOKIE 0x2112A442u
#define ALK_STUN_BINDING_REQUEST 0x0001u
#define ALK_STUN_BINDING_SUCCESS_RESPONSE 0x0101u
#define ALK_STUN_ATTR_MAPPED_ADDRESS 0x0001u
#define ALK_STUN_ATTR_CHANGE_REQUEST 0x0003u
#define ALK_STUN_ATTR_USERNAME 0x0006u
#define ALK_STUN_ATTR_MESSAGE_INTEGRITY 0x0008u
#define ALK_STUN_ATTR_ERROR_CODE 0x0009u
#define ALK_STUN_ATTR_XOR_MAPPED_ADDRESS 0x0020u
#define ALK_STUN_ATTR_PRIORITY 0x0024u
#define ALK_STUN_ATTR_USE_CANDIDATE 0x0025u
#define ALK_STUN_ATTR_MESSAGE_INTEGRITY_SHA256 0x001cu
#define ALK_STUN_ATTR_FINGERPRINT 0x8028u
#define ALK_STUN_ATTR_ICE_CONTROLLED 0x8029u
#define ALK_STUN_ATTR_ICE_CONTROLLING 0x802au
#define ALK_STUN_ATTR_RESPONSE_ORIGIN 0x802bu
#define ALK_STUN_ATTR_OTHER_ADDRESS 0x802cu

typedef enum AlkStunAddressFamily {
  ALK_STUN_ADDRESS_UNKNOWN = 0,
  ALK_STUN_ADDRESS_IPV4 = 4,
  ALK_STUN_ADDRESS_IPV6 = 6,
} AlkStunAddressFamily;

/**
 * A decoded STUN transport address.
 *
 * Address contains 4 bytes for IPv4 or 16 bytes for IPv6, both in network byte
 * order. Port is host byte order.
 */
typedef struct AlkStunMappedEndpoint {
  uint32_t version;        /**< ALK_STUN_MAPPED_ENDPOINT_VERSION. */
  uint32_t address_family; /**< AlkStunAddressFamily. */
  uint16_t port;           /**< Nonzero host-byte-order UDP port. */
  uint16_t reserved;
  uint8_t address[16]; /**< Network-byte-order address bytes. */
} AlkStunMappedEndpoint;

/** Supported short-term MESSAGE-INTEGRITY algorithms. */
typedef enum AlkStunIntegrityAlgorithm {
  ALK_STUN_INTEGRITY_SHA1 = 1,
  ALK_STUN_INTEGRITY_SHA256 = 2,
} AlkStunIntegrityAlgorithm;

/** Password algorithms registered by RFC 8489 for long-term credentials. */
typedef enum AlkStunPasswordAlgorithm {
  ALK_STUN_PASSWORD_MD5 = 1,
  ALK_STUN_PASSWORD_SHA256 = 2,
} AlkStunPasswordAlgorithm;

/**
 * Borrowed view of one validated STUN attribute.
 *
 * Value aliases the input message and remains valid only while that message is
 * alive and unchanged.
 */
typedef struct AlkStunAttributeView {
  uint16_t type;        /**< Attribute type in host byte order. */
  uint16_t length;      /**< Unpadded value bytes. */
  size_t offset;        /**< Attribute-header offset in the message. */
  const uint8_t *value; /**< Borrowed first value byte. */
} AlkStunAttributeView;

/**
 * Caller-owned bounded STUN builder state.
 *
 * The builder borrows storage until the message is sent. It is thread-confined.
 * After MESSAGE-INTEGRITY only FINGERPRINT may be appended.
 */
typedef struct AlkStunMessageBuilder {
  uint8_t *bytes;         /**< Borrowed caller storage. */
  size_t capacity;        /**< Total writable storage bytes. */
  size_t length;          /**< Current complete message bytes. */
  bool integrity_added;   /**< MESSAGE-INTEGRITY ordering guard. */
  bool fingerprint_added; /**< Terminal-attribute ordering guard. */
} AlkStunMessageBuilder;

/** Initialize a decoded endpoint to the current ABI version. */
void alk_stun_mapped_endpoint_init(AlkStunMappedEndpoint *endpoint);

/** Validate the STUN header, exact message length, TLV bounds, and padding. */
bool alk_stun_message_validate(const uint8_t *message, size_t message_length);

/** Find the first attribute of @p type and return a borrowed view. */
bool alk_stun_find_attribute(const uint8_t *message, size_t message_length,
                             uint16_t type, AlkStunAttributeView *attribute);

/** Initialize a message in caller-owned @p storage with a 96-bit transaction
 * ID. */
bool alk_stun_builder_init(AlkStunMessageBuilder *builder, uint8_t *storage,
                           size_t capacity, uint16_t message_type,
                           const uint8_t transaction_id[12]);

/** Append one bounded TLV, adding zero padding to a 32-bit boundary. */
bool alk_stun_builder_add(AlkStunMessageBuilder *builder, uint16_t type,
                          const void *value, uint16_t value_length);

/** Append a network-byte-order unsigned 32-bit attribute value. */
bool alk_stun_builder_add_u32(AlkStunMessageBuilder *builder, uint16_t type,
                              uint32_t value);

/** Append a network-byte-order unsigned 64-bit attribute value. */
bool alk_stun_builder_add_u64(AlkStunMessageBuilder *builder, uint16_t type,
                              uint64_t value);

/** Append a zero-length flag attribute. */
bool alk_stun_builder_add_flag(AlkStunMessageBuilder *builder, uint16_t type);

/** Append RFC 5780 CHANGE-REQUEST flags for diagnostic probes. */
bool alk_stun_builder_add_change_request(AlkStunMessageBuilder *builder,
                                         bool change_ip, bool change_port);

/** Append XOR-MAPPED-ADDRESS using the builder's transaction ID. */
bool alk_stun_builder_add_xor_mapped_endpoint(
    AlkStunMessageBuilder *builder, const AlkStunMappedEndpoint *endpoint);

/** Append an RFC 8489 XOR transport address under @p attribute_type. */
bool alk_stun_builder_add_xor_endpoint(AlkStunMessageBuilder *builder,
                                       uint16_t attribute_type,
                                       const AlkStunMappedEndpoint *endpoint);

/** Append RFC 8489 short-term MESSAGE-INTEGRITY using borrowed key bytes. */
bool alk_stun_builder_add_integrity(AlkStunMessageBuilder *builder,
                                    uint32_t algorithm, const void *key,
                                    size_t key_length);

/** Append the terminal RFC 8489 FINGERPRINT attribute. */
bool alk_stun_builder_add_fingerprint(AlkStunMessageBuilder *builder);

/** Verify MESSAGE-INTEGRITY in constant time; later attributes are excluded. */
bool alk_stun_verify_integrity(const uint8_t *message, size_t message_length,
                               uint32_t algorithm, const void *key,
                               size_t key_length);

/**
 * Derive an RFC 8489 long-term credential key.
 *
 * The three borrowed strings must already satisfy the OpaqueString profile;
 * this byte-level primitive does not perform Unicode preparation. The output
 * is 16 bytes for MD5 compatibility or 32 bytes for SHA-256. On failure no
 * key length is published.
 */
bool alk_stun_derive_long_term_key(uint32_t password_algorithm,
                                   const void *username, size_t username_length,
                                   const void *realm, size_t realm_length,
                                   const void *password, size_t password_length,
                                   uint8_t *key, size_t key_capacity,
                                   size_t *key_length);

/** Verify a terminal FINGERPRINT over the validated message. */
bool alk_stun_verify_fingerprint(const uint8_t *message, size_t message_length);

/** Validate a STUN message and match its type and 96-bit transaction ID. */
bool alk_stun_message_matches(const uint8_t *message, size_t message_length,
                              uint16_t message_type,
                              const uint8_t transaction_id[12]);

/** Decode a validated ERROR-CODE attribute from a matching transaction. */
bool alk_stun_decode_error_code(const uint8_t *message, size_t message_length,
                                const uint8_t transaction_id[12],
                                uint16_t *error_code);

/** Decode an RFC 8489 XOR transport address from a matching transaction. */
bool alk_stun_decode_xor_endpoint(const uint8_t *message, size_t message_length,
                                  const uint8_t transaction_id[12],
                                  uint16_t attribute_type,
                                  AlkStunMappedEndpoint *endpoint);

/**
 * Decode XOR-MAPPED-ADDRESS (preferred) or MAPPED-ADDRESS from a Binding
 * success response after matching @p transaction_id.
 */
bool alk_stun_decode_mapped_endpoint(const uint8_t *response,
                                     size_t response_length,
                                     const uint8_t transaction_id[12],
                                     AlkStunMappedEndpoint *endpoint);

/** Decode RFC 5780 RESPONSE-ORIGIN from a matching Binding response. */
bool alk_stun_decode_response_origin(const uint8_t *response,
                                     size_t response_length,
                                     const uint8_t transaction_id[12],
                                     AlkStunMappedEndpoint *endpoint);

/** Decode RFC 5780 OTHER-ADDRESS from a matching Binding response. */
bool alk_stun_decode_other_address(const uint8_t *response,
                                   size_t response_length,
                                   const uint8_t transaction_id[12],
                                   AlkStunMappedEndpoint *endpoint);

#ifdef __cplusplus
}
#endif

#endif /* ALKAIDLAB_NETWORK_STUN_CODEC_H */
