/**
 * @file src/display_device/color_profile.h
 * @brief Resolve per-client HDR color profile configuration.
 */
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace display_device::color_profile {
  /**
   * Describes how the caller should treat the resolved profile setting.
   *
   * `unspecified` means that no matching client setting (or no hdrProfile
   * field) exists and the caller should leave the current association alone.
   * `clear` represents an explicitly empty hdrProfile field. `apply` always
   * carries a validated profile basename in resolve_result_t::profile.
   */
  enum class profile_policy_e {
    unspecified,
    clear,
    apply,
  };

  struct resolve_result_t {
    profile_policy_e policy { profile_policy_e::unspecified };
    std::optional<std::string> profile;
    bool used_legacy_name { false };
    std::string error;

    explicit operator bool() const noexcept {
      return error.empty();
    }
  };

  /**
   * Validate a profile filename accepted by the Windows Advanced Color API.
   * Paths are intentionally rejected; profiles must already be installed in
   * the system color directory.
   */
  [[nodiscard]] bool
  is_valid_profile_basename(std::string_view profile) noexcept;

  /**
   * Resolve a client's HDR profile from the serialized clients array.
   *
   * A non-empty UUID is authoritative. The legacy name is considered only
   * when the UUID is empty, and only a unique matching name is accepted.
   * Profile values are restricted to safe .icc/.icm basenames.
   */
  [[nodiscard]] resolve_result_t
  resolve_client_hdr_profile(
    std::string_view clients_json,
    std::string_view client_uuid,
    std::string_view legacy_name
  );
}  // namespace display_device::color_profile
