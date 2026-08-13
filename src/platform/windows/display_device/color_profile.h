/**
 * @file src/platform/windows/display_device/color_profile.h
 * @brief Reversible Windows Advanced Color profile associations.
 */
#pragma once

// standard includes
#include <cstdint>
#include <optional>
#include <string>

// lib includes
#include <nlohmann/json_fwd.hpp>

namespace display_device::win_color_profile {

  /**
   * @brief Persistent state needed to undo one Advanced Color profile change.
   *
   * Profile names are the installed profile names understood by Windows Color
   * System, not paths to arbitrary ICC files. The display is deliberately
   * stored as Sunshine's stable device id; every operation resolves the active
   * DISPLAYCONFIG path again so a persisted snapshot does not retain stale
   * adapter/source identifiers.
   */
  struct state_t {
    static constexpr std::uint32_t current_version { 1 };

    std::uint32_t version { current_version };
    std::string device_id;
    std::string original_default_profile;
    std::string applied_profile;
    bool original_default_present { false };
    bool original_scope_was_current_user { false };
    bool applied_profile_was_associated { false };
  };

  struct calibration_luminance_t {
    std::string profile_name;
    float max_nits {};
    float min_nits {};
    float max_full_frame_nits {};
  };

  /** Read ST.2086 luminance from the display's active MHC2 profile. */
  [[nodiscard]] std::optional<calibration_luminance_t>
  current_hdr_calibration(const std::string &device_id);

  /**
   * @brief Serialize a profile restoration snapshot.
   */
  void
  to_json(nlohmann::json &json, const state_t &state);

  /**
   * @brief Deserialize a profile restoration snapshot.
   *
   * Missing fields retain their default values, which keeps persisted state
   * readable when fields are added in later releases.
   */
  void
  from_json(const nlohmann::json &json, state_t &state);

  /**
   * @brief Snapshot the active display's current Extended/Advanced Color state.
   * @param device_id Sunshine display device id to resolve.
   * @param applied_profile Installed ICC profile that apply() will select.
   * @returns Serializable restoration state, or std::nullopt on failure.
   */
  [[nodiscard]] std::optional<state_t>
  snapshot(const std::string &device_id, const std::string &applied_profile);

  /**
   * @brief Associate and select the snapshot's profile for Advanced Color.
   * @returns true only when the Windows Color System operation succeeded.
   */
  [[nodiscard]] bool
  apply(const state_t &state);

  /**
   * @brief Restore the default and association state captured by snapshot().
   * @returns true only when the complete restoration succeeded.
   */
  [[nodiscard]] bool
  restore(const state_t &state);

}  // namespace display_device::win_color_profile
