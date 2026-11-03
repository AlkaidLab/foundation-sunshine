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

#include "src/hdr/calibration.h"

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

  struct calibration_result_t {
    enum class state_e {
      absent,
      unavailable,
      invalid,
      detected
    };
    state_e state { state_e::unavailable };
    std::string stage;
    std::uint32_t error {};
    std::string user_sid;
    std::optional<unsigned> scope;
    std::optional<hdr::calibration_profile_t> profile;
  };

  /** Inspect the exact display's active HDR association as the interactive user. */
  [[nodiscard]] calibration_result_t
  current_hdr_calibration(const std::string &device_id);

  /** Validate an installed bound profile before the client's VDD exists. */
  [[nodiscard]] calibration_result_t
  read_hdr_calibration(const std::string &profile_name);

  /** Select an already-validated binding on the exact active VDD, without touching physical screens. */
  bool
  apply_hdr_calibration(const std::string &device_id, const hdr::calibration_binding_t &binding);

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
