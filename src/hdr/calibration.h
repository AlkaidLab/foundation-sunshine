/**
 * @file src/hdr/calibration.h
 * @brief Validated ICC luminance and per-client calibration ownership.
 */
#pragma once

#include "client_display_capabilities.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <nlohmann/json_fwd.hpp>

namespace hdr {
  struct calibration_profile_t {
    std::string profile_name;
    std::string fingerprint;
    client_display_capabilities_t luminance;
  };

  struct calibration_binding_t {
    std::string user_sid;
    std::string vdd_id;
    calibration_profile_t profile;
  };

  /** Parse complete ICC v2/v4 display profiles, respecting each tag's bounds. */
  std::optional<client_display_capabilities_t>
  parse_calibration_luminance(std::span<const std::uint8_t> bytes);

  bool
  same_calibration(const calibration_profile_t &left, const calibration_profile_t &right);

  bool
  calibration_matches(const calibration_binding_t &binding, std::string_view user_sid,
    std::string_view vdd_id, const calibration_profile_t &profile);

  void
  to_json(nlohmann::json &json, const calibration_binding_t &binding);
  void
  from_json(const nlohmann::json &json, calibration_binding_t &binding);

  /** UUID matching is deliberately strict; calibration never falls back to a name. */
  std::optional<calibration_binding_t>
  client_calibration(std::string_view clients, std::string_view uuid);

  /** Retain Core-owned bindings when a settings form saves an older client snapshot. */
  std::string
  preserve_client_calibrations(std::string_view incoming, std::string_view existing);
}  // namespace hdr
