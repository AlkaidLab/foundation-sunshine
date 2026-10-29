/**
 * @file src/display_device/vdd_settings.h
 * @brief Persist virtual display modes and the native EDID preference.
 */
#pragma once

#include "vdd_edid_resolution.h"

#include <string>

namespace display_device::vdd_settings {
  enum class save_result {
    saved,
    missing_file,
    failed,
  };

  /** Distinguish an absent VDD configuration from an unsuccessful write. */
  save_result
  save(std::string resArray, std::string fpsArray, std::string gpu_name, const std::string &edid_native_resolution = "");
}  // namespace display_device::vdd_settings
