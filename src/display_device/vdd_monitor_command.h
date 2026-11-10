/**
 * @file src/display_device/vdd_monitor_command.h
 * @brief Build the existing VDD create command with an optional native EDID size.
 */
#pragma once

#include "vdd_edid_resolution.h"

#include <locale>
#include <sstream>
#include <string>
#include <string_view>

namespace display_device::vdd_edid {
  /**
   * @brief Build a locale-independent create command, retaining legacy parameter positions.
   * @details Default/malformed sizes omit the third group. Explicit sizes reserve
   *          the physical-size group even without a GUID; mode membership is checked by VDD.
   */
  inline std::string
  create_monitor_command(std::string_view guid, float max_nits, float min_nits, float max_fall,
    float width_cm, float height_cm, std::string_view native_resolution) {
    const bool explicit_size = native_resolution != "default" && valid_value(native_resolution);
    std::ostringstream command;
    command.imbue(std::locale::classic());
    command << "CREATEMONITOR";
    if (!guid.empty() || explicit_size) {
      command << ' ' << guid << ":[" << max_nits << ',' << min_nits << ',' << max_fall << ']';
      const bool physical_size = width_cm > 0 && height_cm > 0;
      if (physical_size || explicit_size) {
        // Keep the existing positional physical-size group, including when unset.
        command << '[' << (physical_size ? width_cm : 0) << ',' << (physical_size ? height_cm : 0) << ']';
      }
      if (explicit_size) command << "[edid=" << native_resolution << ']';
    }
    return command.str();
  }
}  // namespace display_device::vdd_edid
