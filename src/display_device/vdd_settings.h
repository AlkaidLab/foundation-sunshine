/**
 * @file src/display_device/vdd_settings.h
 * @brief Persist the virtual display resolution and refresh-rate settings.
 */
#pragma once

#include <string>
#ifdef SUNSHINE_TESTS
  #include <filesystem>
  #include <optional>
#endif

namespace display_device::vdd_settings {
  bool
  save(std::string resArray, std::string fpsArray, std::string gpu_name);
#ifdef SUNSHINE_TESTS
  namespace test_support {
    /** Require an explicit fixture path; nullopt disables XML writes in test builds. */
    void
    set_settings_path(const std::optional<std::filesystem::path> &path);
  }  // namespace test_support
#endif
}  // namespace display_device::vdd_settings
