/**
 * @file src/display_device/vdd_settings.h
 * @brief Persist the virtual display resolution and refresh-rate settings.
 */
#pragma once

#include <string>

namespace display_device::vdd_settings {
  bool
  save(std::string resArray, std::string fpsArray, std::string gpu_name);
}  // namespace display_device::vdd_settings
