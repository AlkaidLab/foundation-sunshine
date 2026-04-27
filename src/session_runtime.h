/**
 * @file src/session_runtime.h
 * @brief Shared runtime session identity primitives.
 */
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace session_runtime {

  inline std::uint64_t
  stable_key(std::string_view value) {
    constexpr std::uint64_t fnv_offset = 14695981039346656037ull;
    constexpr std::uint64_t fnv_prime = 1099511628211ull;

    std::uint64_t hash = fnv_offset;
    for (unsigned char ch : value) {
      hash ^= ch;
      hash *= fnv_prime;
    }

    return hash;
  }

  enum class feature_e : std::uint8_t {
    clipboard,
    microphone,
    display,
    dynamic_params,
    input_focus,
    transport_qos,
    cursor_plane,
    clipboard_bulk,
    dynamic_quality,
  };

  enum class resource_scope_e : std::uint8_t {
    per_session,
    per_device,
    shared_global,
    global_exclusive,
  };

  enum class display_allocation_mode_e : std::uint8_t {
    shared_owner,
    shared_follower,
    dedicated,
  };

  struct identity_t {
    std::uint64_t runtime_id {};
    std::uint64_t client_cert_key {};
    std::uint32_t launch_session_id {};
    std::uint32_t control_generation {};
    std::uint32_t control_connect_data {};
    std::string client_cert_uuid;
    std::string client_unique_id;
    std::string client_name;
    std::string av_ping_payload;

    bool
    has_trusted_client_identity() const {
      return !client_cert_uuid.empty();
    }

    void
    set_client_cert_uuid(std::string uuid) {
      client_cert_uuid = std::move(uuid);
      client_cert_key = client_cert_uuid.empty() ? 0 : stable_key(client_cert_uuid);
    }
  };

  struct owner_token_t {
    feature_e feature {};
    std::uint64_t runtime_id {};
    std::uint64_t client_cert_key {};
    std::uint32_t control_generation {};

    explicit operator bool() const {
      return runtime_id != 0;
    }
  };

  struct display_allocation_t {
    owner_token_t owner { feature_e::display };
    resource_scope_e scope { resource_scope_e::shared_global };
    display_allocation_mode_e mode { display_allocation_mode_e::shared_owner };
    std::uint32_t resource_slot {};
  };

}  // namespace session_runtime
