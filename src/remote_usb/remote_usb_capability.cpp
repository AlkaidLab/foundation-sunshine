/**
 * @file src/remote_usb/remote_usb_capability.cpp
 * @brief One-time capability store implementation.
 */

#include "remote_usb_capability.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <openssl/rand.h>
#include <stdexcept>
#include <utility>

namespace remote_usb {
namespace {

constexpr std::string_view kBase64UrlAlphabet {
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_" };

std::optional<std::uint8_t>
base64_value(char value) noexcept {
  if (value >= 'A' && value <= 'Z') {
    return static_cast<std::uint8_t>(value - 'A');
  }
  if (value >= 'a' && value <= 'z') {
    return static_cast<std::uint8_t>(value - 'a' + 26);
  }
  if (value >= '0' && value <= '9') {
    return static_cast<std::uint8_t>(value - '0' + 52);
  }
  if (value == '-') {
    return 62;
  }
  if (value == '_') {
    return 63;
  }
  return std::nullopt;
}

}  // namespace

capability_store::capability_store():
    capability_store(policy {}) {
}

capability_store::capability_store(policy policy):
    policy_(std::move(policy)) {
}

bool
capability_store::valid_client_uuid(std::string_view client_uuid) noexcept {
  /* Pairing UUIDs are currently 16 ASCII hex bytes.  Keep the store tolerant
   * of future canonical forms while rejecting control/NUL bytes and absurdly
   * large values that could be used for memory pressure. */
  if (client_uuid.empty() || client_uuid.size() > 128) {
    return false;
  }
  return std::all_of(client_uuid.begin(), client_uuid.end(), [](unsigned char c) {
    return c >= 0x20u && c <= 0x7eu;
  });
}

bool
capability_store::valid_endpoint(const capability_endpoint &endpoint) noexcept {
  if (endpoint.host.empty() || endpoint.host.size() > 255 || endpoint.port == 0) {
    return false;
  }
  return std::all_of(endpoint.host.begin(), endpoint.host.end(), [](unsigned char c) {
    return c >= 0x21u && c <= 0x7eu;
  });
}

bool
capability_store::valid_wire_client_uuid(std::string_view wire_client_uuid) noexcept {
  if (wire_client_uuid.empty() || wire_client_uuid.size() > 64) {
    return false;
  }
  /* A fixed 16-byte HELLO identity may be an opaque binary value.  Textual
   * identities used by the current Android client are validated below. */
  if (wire_client_uuid.size() == capability_wire_identity_size) {
    return !std::all_of(wire_client_uuid.begin(), wire_client_uuid.end(), [](unsigned char c) {
      return c == 0;
    });
  }
  return std::all_of(wire_client_uuid.begin(), wire_client_uuid.end(), [](unsigned char c) {
    return c >= 0x20u && c <= 0x7eu;
  });
}

bool
capability_store::constant_time_equal(std::string_view lhs,
                                       std::string_view rhs) noexcept {
  const auto max_size = std::max(lhs.size(), rhs.size());
  std::uint64_t difference = static_cast<std::uint64_t>(lhs.size() ^ rhs.size());
  for (std::size_t i = 0; i < max_size; ++i) {
    const auto left = i < lhs.size() ? static_cast<std::uint8_t>(lhs[i]) : 0;
    const auto right = i < rhs.size() ? static_cast<std::uint8_t>(rhs[i]) : 0;
    difference |= static_cast<std::uint64_t>(left ^ right);
  }
  return difference == 0;
}

bool
capability_store::constant_time_equal(
  const std::array<std::uint8_t, capability_nonce_size> &lhs,
  const std::array<std::uint8_t, capability_nonce_size> &rhs) noexcept {
  std::uint8_t difference = 0;
  for (std::size_t i = 0; i < capability_nonce_size; ++i) {
    difference = static_cast<std::uint8_t>(difference | (lhs[i] ^ rhs[i]));
  }
  return difference == 0;
}

std::string
capability_store::nonce_key(
  const std::array<std::uint8_t, capability_nonce_size> &nonce) {
  return std::string {
    reinterpret_cast<const char *>(nonce.data()), nonce.size() };
}

std::array<std::uint8_t, capability_nonce_size>
capability_store::random_nonce() {
  std::array<std::uint8_t, capability_nonce_size> nonce {};
  do {
    if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1) {
      throw std::runtime_error("remote USB capability: RAND_bytes failed");
    }
  } while (std::all_of(nonce.begin(), nonce.end(), [](std::uint8_t byte) {
    return byte == 0;
  }));
  return nonce;
}

std::optional<capability>
capability_store::issue(std::string client_uuid,
                        std::uint64_t stream_generation,
                        capability_endpoint endpoint,
                        clock_t::time_point now) {
  return issue_impl(std::move(client_uuid),
                    stream_generation,
                    std::move(endpoint),
                    std::string {},
                    0,
                    0,
                    0,
                    false,
                    now);
}

std::optional<capability>
capability_store::issue(std::string client_uuid,
                        std::uint64_t stream_generation,
                        capability_endpoint endpoint,
                        std::string wire_client_uuid,
                        clock_t::time_point now) {
  return issue_impl(std::move(client_uuid),
                    stream_generation,
                    std::move(endpoint),
                    std::move(wire_client_uuid),
                    0,
                    0,
                    0,
                    false,
                    now);
}

std::optional<capability>
capability_store::issue(std::string client_uuid,
                        std::uint64_t stream_generation,
                        capability_endpoint endpoint,
                        std::uint64_t session_token,
                        std::uint64_t attachment_token,
                        std::uint64_t lease_token,
                        clock_t::time_point now) {
  return issue_impl(std::move(client_uuid),
                    stream_generation,
                    std::move(endpoint),
                    std::string {},
                    session_token,
                    attachment_token,
                    lease_token,
                    true,
                    now);
}

std::optional<capability>
capability_store::issue(std::string client_uuid,
                        std::uint64_t stream_generation,
                        capability_endpoint endpoint,
                        std::string wire_client_uuid,
                        std::uint64_t session_token,
                        std::uint64_t attachment_token,
                        std::uint64_t lease_token,
                        clock_t::time_point now) {
  return issue_impl(std::move(client_uuid),
                    stream_generation,
                    std::move(endpoint),
                    std::move(wire_client_uuid),
                    session_token,
                    attachment_token,
                    lease_token,
                    true,
                    now);
}

std::optional<capability>
capability_store::issue_impl(std::string client_uuid,
                             std::uint64_t stream_generation,
                             capability_endpoint endpoint,
                             std::string wire_client_uuid,
                             std::uint64_t session_token,
                             std::uint64_t attachment_token,
                             std::uint64_t lease_token,
                             bool require_token_binding,
                             clock_t::time_point now) {
  if (!valid_client_uuid(client_uuid) || stream_generation == 0 ||
      !valid_endpoint(endpoint) || policy_.ttl <= std::chrono::milliseconds::zero() ||
      (!wire_client_uuid.empty() && !valid_wire_client_uuid(wire_client_uuid)) ||
      (require_token_binding &&
       (session_token == 0 || attachment_token == 0 || lease_token == 0)) ||
      policy_.max_urb < capability_min_max_urb ||
      policy_.max_urb > capability_default_max_pdu_size ||
      policy_.max_inflight == 0 || policy_.max_inflight > capability_default_max_inflight) {
    return std::nullopt;
  }

  std::scoped_lock lock { mutex_ };
  cleanup_unlocked(now);
  if ((policy_.max_entries != 0 && records_.size() >= policy_.max_entries) ||
      (policy_.max_entries_per_client != 0 &&
       count_client_unlocked(client_uuid) >= policy_.max_entries_per_client)) {
    return std::nullopt;
  }

  capability value;
  value.client_uuid = std::move(client_uuid);
  value.wire_client_uuid = std::move(wire_client_uuid);
  value.stream_generation = stream_generation;
  value.session_token = session_token;
  value.attachment_token = attachment_token;
  value.lease_token = lease_token;
  value.endpoint = std::move(endpoint);
  value.max_urb = policy_.max_urb;
  value.max_inflight = policy_.max_inflight;
  const auto ttl_ms = policy_.ttl.count();
  value.expires_ms = static_cast<std::uint32_t>(std::min<std::int64_t>(
    std::max<std::int64_t>(ttl_ms, 1), std::numeric_limits<std::uint32_t>::max()));

  try {
    do {
      value.nonce = random_nonce();
    } while (records_.contains(nonce_key(value.nonce)));
  }
  catch (const std::exception &) {
    return std::nullopt;
  }

  records_.emplace(nonce_key(value.nonce), record { value, now + policy_.ttl });
  return value;
}

std::optional<capability>
capability_store::consume(
  std::string_view client_uuid,
  std::uint64_t stream_generation,
  const std::array<std::uint8_t, capability_nonce_size> &nonce,
  clock_t::time_point now) {
  return consume_impl(client_uuid,
                      stream_generation,
                      0,
                      0,
                      0,
                      nonce,
                      false,
                      now);
}

std::optional<capability>
capability_store::consume(
  std::string_view client_uuid,
  std::uint64_t stream_generation,
  std::uint64_t session_token,
  std::uint64_t attachment_token,
  std::uint64_t lease_token,
  const std::array<std::uint8_t, capability_nonce_size> &nonce,
  clock_t::time_point now) {
  return consume_impl(client_uuid,
                      stream_generation,
                      session_token,
                      attachment_token,
                      lease_token,
                      nonce,
                      true,
                      now);
}

std::optional<capability>
capability_store::consume_impl(
  std::string_view client_uuid,
  std::uint64_t stream_generation,
  std::uint64_t session_token,
  std::uint64_t attachment_token,
  std::uint64_t lease_token,
  const std::array<std::uint8_t, capability_nonce_size> &nonce,
  bool require_token_binding,
  clock_t::time_point now) {
  std::scoped_lock lock { mutex_ };
  cleanup_unlocked(now);

  auto it = records_.find(nonce_key(nonce));
  if (it == records_.end()) {
    return std::nullopt;
  }

  /* Erase before returning so an exception in a caller cannot leave a replayable
   * capability.  Expired records have already been removed by cleanup. */
  auto value = it->second.value;
  records_.erase(it);
  if (value.stream_generation != stream_generation ||
      !constant_time_equal(value.client_uuid, client_uuid) ||
      (require_token_binding &&
       !matches_tokens(value, session_token, attachment_token, lease_token))) {
    return std::nullopt;
  }
  return value;
}

bool
capability_store::matches_tokens(const capability &capability,
                                 std::uint64_t session_token,
                                 std::uint64_t attachment_token,
                                 std::uint64_t lease_token) noexcept {
  /* Treat an all-zero record as unbound rather than as a wildcard.  This is
   * what lets old callers keep compiling while ensuring the broker's new
   * token-aware path fails closed if they have not been upgraded. */
  if (capability.session_token == 0 || capability.attachment_token == 0 ||
      capability.lease_token == 0 || session_token == 0 || attachment_token == 0 ||
      lease_token == 0) {
    return false;
  }
  const auto difference = (capability.session_token ^ session_token) |
                          (capability.attachment_token ^ attachment_token) |
                          (capability.lease_token ^ lease_token);
  return difference == 0;
}

bool
capability_store::matches_wire_identity(const capability &capability,
                                         std::string_view wire_client_uuid) noexcept {
  if (capability.wire_client_uuid.empty()) {
    return true;
  }
  return constant_time_equal(capability.wire_client_uuid, wire_client_uuid);
}

bool
capability_store::matches_wire_identity(
  const capability &capability,
  const std::array<std::uint8_t, capability_wire_identity_size> &wire_client_uuid) noexcept {
  if (capability.wire_client_uuid.empty()) {
    return true;
  }

  /* A 16-character ASCII hex identity is the v1 Android convention.  A
   * non-hex 16-byte value is an opaque binary identity and must be compared
   * byte-for-byte; longer textual values use the uppercase hex rendering of
   * the fixed-width wire field.  This keeps the two representations distinct
   * without relying on an overload that would be identical at compile time.
   */
  const auto capability_is_ascii_hex = capability.wire_client_uuid.size() ==
                                       capability_wire_identity_size &&
    std::all_of(capability.wire_client_uuid.begin(), capability.wire_client_uuid.end(),
                [](unsigned char byte) { return std::isxdigit(byte) != 0; });
  if (capability.wire_client_uuid.size() == capability_wire_identity_size &&
      !capability_is_ascii_hex) {
    const std::string raw(reinterpret_cast<const char *>(wire_client_uuid.data()),
                          wire_client_uuid.size());
    return constant_time_equal(capability.wire_client_uuid, raw);
  }

  const bool wire_is_ascii_hex = std::all_of(
    wire_client_uuid.begin(), wire_client_uuid.end(),
    [](unsigned char byte) { return std::isxdigit(byte) != 0; });
  std::string canonical;
  if (wire_is_ascii_hex && capability.wire_client_uuid.size() == wire_client_uuid.size()) {
    canonical.reserve(wire_client_uuid.size());
    for (const auto byte : wire_client_uuid) {
      canonical.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(byte))));
    }
  }
  else {
    static constexpr char hex[] = "0123456789ABCDEF";
    canonical.reserve(wire_client_uuid.size() * 2);
    for (const auto byte : wire_client_uuid) {
      canonical.push_back(hex[(byte >> 4) & 0x0Fu]);
      canonical.push_back(hex[byte & 0x0Fu]);
    }
  }
  return constant_time_equal(capability.wire_client_uuid, canonical);
}

bool
capability_store::revoke(
  const std::array<std::uint8_t, capability_nonce_size> &nonce) {
  std::scoped_lock lock { mutex_ };
  return records_.erase(nonce_key(nonce)) != 0;
}

void
capability_store::cleanup(clock_t::time_point now) {
  std::scoped_lock lock { mutex_ };
  cleanup_unlocked(now);
}

void
capability_store::clear() {
  std::scoped_lock lock { mutex_ };
  records_.clear();
}

void
capability_store::cleanup_unlocked(clock_t::time_point now) {
  for (auto it = records_.begin(); it != records_.end();) {
    if (it->second.expires_at <= now) {
      it = records_.erase(it);
    }
    else {
      ++it;
    }
  }
}

std::size_t
capability_store::count_client_unlocked(std::string_view client_uuid) const {
  std::size_t count = 0;
  for (const auto &[_, value] : records_) {
    if (constant_time_equal(value.value.client_uuid, client_uuid)) {
      ++count;
    }
  }
  return count;
}

std::size_t
capability_store::size() const {
  std::scoped_lock lock { mutex_ };
  return records_.size();
}

std::string
capability_store::encode_nonce(
  const std::array<std::uint8_t, capability_nonce_size> &nonce) {
  std::string out;
  out.reserve((nonce.size() * 8 + 5) / 6);
  std::uint32_t accumulator = 0;
  unsigned bits = 0;
  for (const auto byte : nonce) {
    accumulator = (accumulator << 8) | byte;
    bits += 8;
    while (bits >= 6) {
      bits -= 6;
      out.push_back(kBase64UrlAlphabet[(accumulator >> bits) & 0x3fu]);
    }
  }
  if (bits != 0) {
    out.push_back(kBase64UrlAlphabet[(accumulator << (6 - bits)) & 0x3fu]);
  }
  return out;
}

std::optional<std::array<std::uint8_t, capability_nonce_size>>
capability_store::decode_nonce(std::string_view encoded) {
  /* 16 bytes encode to exactly 22 unpadded base64url characters. */
  if (encoded.size() != 22) {
    return std::nullopt;
  }

  std::array<std::uint8_t, capability_nonce_size> output {};
  std::uint32_t accumulator = 0;
  unsigned bits = 0;
  std::size_t output_index = 0;
  for (const auto character : encoded) {
    const auto value = base64_value(character);
    if (!value) {
      return std::nullopt;
    }
    accumulator = (accumulator << 6) | *value;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (output_index >= output.size()) {
        return std::nullopt;
      }
      output[output_index++] = static_cast<std::uint8_t>((accumulator >> bits) & 0xffu);
    }
  }
  if (output_index != output.size() || bits != 4 ||
      (accumulator & ((1u << bits) - 1u)) != 0) {
    return std::nullopt;
  }
  if (std::all_of(output.begin(), output.end(), [](std::uint8_t byte) { return byte == 0; })) {
    return std::nullopt;
  }
  return output;
}

}  // namespace remote_usb
