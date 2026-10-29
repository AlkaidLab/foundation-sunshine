/**
 * @file src/display_device/vdd_settings.h
 * @brief Persist virtual display modes and the native EDID preference.
 */
#pragma once

#include "vdd_edid_resolution.h"

#include <string>

namespace display_device::vdd_settings {
  bool
  save(std::string resArray, std::string fpsArray, std::string gpu_name, const std::string &edid_native_resolution = "");
}  // namespace display_device::vdd_settings
