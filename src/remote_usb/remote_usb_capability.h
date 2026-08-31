/**
 * @file src/remote_usb/remote_usb_capability.h
 * @brief One-time capabilities used to bootstrap an authenticated USB broker.
 *
 * The capability is deliberately separate from the RUSB wire protocol.  It is
 * issued by the certificate-authenticated HTTP endpoint and consumed by the
 * first broker HELLO.  A capability is bound to a paired client identity and
 * stream generation and can be consumed at most once.
 */
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace remote_usb {

inline constexpr std::size_t capability_nonce_size = 16;
inline constexpr std::size_t capability_wire_identity_size = 16;
/** USB/IP URB limits use complete PDU sizes, including the 48-byte header. */
inline constexpr std::size_t capability_usbip_header_size = 48u;
inline constexpr std::uint32_t capability_default_max_pdu_size = 1024u * 1024u;
inline constexpr std::uint32_t capability_min_max_urb =
  static_cast<std::uint32_t>(capability_usbip_header_size + 1u);
/* Source compatibility for callers using the original field terminology. */
inline constexpr std::uint32_t capability_default_max_urb = capability_default_max_pdu_size;
inline constexpr std::uint32_t capability_default_max_inflight = 4096u;
inline constexpr std::uint32_t capability_default_expires_ms = 15000u;

struct capability_endpoint {
  std::string host;
  std::uint16_t port { 0 };
};

/** A capability returned to the HTTP caller and later bound to a HELLO. */
struct capability {
  /** UUID resolved from the authenticated client certificate. */
  std::string client_uuid;
  /**
   * Optional fixed-width identity echoed by the RUSB HELLO.  This is kept
   * separate because Moonlight's persistent uniqueid and Sunshine's pairing
   * certificate UUID are not necessarily the same value.  It is never sent
   * back in the HTTP response.
   */
  std::string wire_client_uuid;
  std::uint64_t stream_generation { 0 };
  std::array<std::uint8_t, capability_nonce_size> nonce {};
  capability_endpoint endpoint;
  std::uint32_t max_urb { capability_default_max_urb };
  std::uint32_t max_inflight { capability_default_max_inflight };
  std::uint32_t expires_ms { capability_default_expires_ms };
  /**
   * Stream/attachment/lease tuple expected in the first broker HELLO.  These
   * fields are appended to preserve aggregate/source compatibility with the
   * original capability layout.  The legacy issue() overloads leave them
   * zero; the broker deliberately rejects such unbound records.
   */
  std::uint64_t session_token { 0 };
  std::uint64_t attachment_token { 0 };
  std::uint64_t lease_token { 0 };
};

/**
 * Thread-safe, bounded, one-time capability store.
 *
 * The default TTL and limits are intentionally short and conservative.  The
 * constructor accepts a custom policy so expiry and capacity can be tested
 * without sleeping.
 */
class capability_store final {
public:
  using clock_t = std::chrono::steady_clock;

  struct policy {
    std::chrono::milliseconds ttl { capability_default_expires_ms };
    std::uint32_t max_urb { capability_default_max_urb };
    std::uint32_t max_inflight { capability_default_max_inflight };
    std::size_t max_entries { 256 };
    std::size_t max_entries_per_client { 4 };
  };

  capability_store();
  explicit capability_store(policy policy);

  /** Issue a new capability, or nullopt when the input/policy is invalid. */
  std::optional<capability> issue(
    std::string client_uuid,
    std::uint64_t stream_generation,
    capability_endpoint endpoint,
    clock_t::time_point now = clock_t::now());

  /** Issue a capability with an independent expected RUSB wire identity. */
  std::optional<capability> issue(
    std::string client_uuid,
    std::uint64_t stream_generation,
    capability_endpoint endpoint,
    std::string wire_client_uuid,
    clock_t::time_point now = clock_t::now());

  /**
   * Issue a capability bound to the complete RUSB lease tuple.  All three
   * token values must be non-zero.  This is the production entry point used
   * by the authenticated HTTP route.
   */
  std::optional<capability> issue(
    std::string client_uuid,
    std::uint64_t stream_generation,
    capability_endpoint endpoint,
    std::uint64_t session_token,
    std::uint64_t attachment_token,
    std::uint64_t lease_token,
    clock_t::time_point now = clock_t::now());

  /** Issue a token-bound capability with an independent HELLO wire identity. */
  std::optional<capability> issue(
    std::string client_uuid,
    std::uint64_t stream_generation,
    capability_endpoint endpoint,
    std::string wire_client_uuid,
    std::uint64_t session_token,
    std::uint64_t attachment_token,
    std::uint64_t lease_token,
    clock_t::time_point now = clock_t::now());

  /**
   * Atomically validate and consume a capability.  Any record found for the
   * nonce is removed even when the identity or generation does not match;
   * this prevents an attacker from probing a live nonce and guarantees that a
   * failed HELLO cannot be retried with a different identity.
   */
  std::optional<capability> consume(
    std::string_view client_uuid,
    std::uint64_t stream_generation,
    const std::array<std::uint8_t, capability_nonce_size> &nonce,
    clock_t::time_point now = clock_t::now());

  /**
   * Atomically validate and consume a token-bound capability.  The nonce is
   * consumed even when any token mismatches, just like the legacy overload.
   */
  std::optional<capability> consume(
    std::string_view client_uuid,
    std::uint64_t stream_generation,
    std::uint64_t session_token,
    std::uint64_t attachment_token,
    std::uint64_t lease_token,
    const std::array<std::uint8_t, capability_nonce_size> &nonce,
    clock_t::time_point now = clock_t::now());

  /** Compare the complete token tuple in constant-time style. */
  static bool matches_tokens(
    const capability &capability,
    std::uint64_t session_token,
    std::uint64_t attachment_token,
    std::uint64_t lease_token) noexcept;

  /** Constant-time comparison for the optional HELLO wire identity. */
  static bool matches_wire_identity(
    const capability &capability,
    std::string_view wire_client_uuid) noexcept;

  /** Compare a fixed-width HELLO identity using the canonical wire form. */
  static bool matches_wire_identity(
    const capability &capability,
    const std::array<std::uint8_t, capability_wire_identity_size> &wire_client_uuid) noexcept;

  /** Revoke one capability without consuming it. */
  bool revoke(const std::array<std::uint8_t, capability_nonce_size> &nonce);

  /** Remove expired records. */
  void cleanup(clock_t::time_point now = clock_t::now());

  /** Revoke every outstanding capability, normally when the broker stops. */
  void clear();

  std::size_t size() const;

  /** Base64url without padding, suitable for the JSON capability response. */
  static std::string encode_nonce(
    const std::array<std::uint8_t, capability_nonce_size> &nonce);

  /** Decode an unpadded base64url nonce; malformed or wrong-length input fails. */
  static std::optional<std::array<std::uint8_t, capability_nonce_size>> decode_nonce(
    std::string_view encoded);

private:
  struct record {
    capability value;
    clock_t::time_point expires_at;
  };

  static bool valid_client_uuid(std::string_view client_uuid) noexcept;
  static bool valid_endpoint(const capability_endpoint &endpoint) noexcept;
  static bool valid_wire_client_uuid(std::string_view wire_client_uuid) noexcept;
  static bool constant_time_equal(
    std::string_view lhs,
    std::string_view rhs) noexcept;
  static bool constant_time_equal(
    const std::array<std::uint8_t, capability_nonce_size> &lhs,
    const std::array<std::uint8_t, capability_nonce_size> &rhs) noexcept;

  static std::string nonce_key(
    const std::array<std::uint8_t, capability_nonce_size> &nonce);
  static std::array<std::uint8_t, capability_nonce_size> random_nonce();

  std::optional<capability> issue_impl(
    std::string client_uuid,
    std::uint64_t stream_generation,
    capability_endpoint endpoint,
    std::string wire_client_uuid,
    std::uint64_t session_token,
    std::uint64_t attachment_token,
    std::uint64_t lease_token,
    bool require_token_binding,
    clock_t::time_point now);

  std::optional<capability> consume_impl(
    std::string_view client_uuid,
    std::uint64_t stream_generation,
    std::uint64_t session_token,
    std::uint64_t attachment_token,
    std::uint64_t lease_token,
    const std::array<std::uint8_t, capability_nonce_size> &nonce,
    bool require_token_binding,
    clock_t::time_point now);

  void cleanup_unlocked(clock_t::time_point now);
  std::size_t count_client_unlocked(std::string_view client_uuid) const;

  policy policy_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, record> records_;
};

}  // namespace remote_usb
