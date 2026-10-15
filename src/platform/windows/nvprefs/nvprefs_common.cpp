/**
 * @file src/platform/windows/nvprefs/nvprefs_common.cpp
 * @brief Definitions for common nvidia preferences.
 */
// local includes
#include "nvprefs_common.h"
#include "src/logging.h"
#include <algorithm>
#include <cctype>

// read user override preferences from global sunshine config
#include "src/config.h"

namespace nvprefs {

  void
  info_message(const std::wstring &message) {
    BOOST_LOG(info) << "nvprefs: " << message;
  }

  void
  info_message(const std::string &message) {
    BOOST_LOG(info) << "nvprefs: " << message;
  }

  void
  error_message(const std::wstring &message) {
    BOOST_LOG(error) << "nvprefs: " << message;
  }

  void
  error_message(const std::string &message) {
    BOOST_LOG(error) << "nvprefs: " << message;
  }

  nvprefs_options
  get_nvprefs_options() {
    nvprefs_options options;
    options.manage_driver_settings = config::video.nv_manage_driver_settings;
    options.vdd_force_vsync = config::video.nv_vdd_force_vsync;
    options.opengl_vulkan_on_dxgi = config::video.nv_opengl_vulkan_on_dxgi;
    options.sunshine_high_power_mode = config::video.nv_sunshine_high_power_mode;
    // These lifecycle settings apply to the next launch after saving, without
    // restarting Sunshine or touching a running game's presentation state.
    if (const auto snapshot = config::get_effective_config_snapshot()) {
      const auto enabled = [&](const char *name, bool fallback) {
        const auto it = snapshot->find(name);
        if (it == snapshot->end() || it->second.empty()) return fallback;
        auto value = it->second;
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) { return std::tolower(ch); });
        return value == "true" || value == "yes" || value == "enable" ||
               value == "enabled" || value == "on" || value == "1";
      };
      options.manage_driver_settings = enabled("nvenc_manage_driver_settings", true);
      options.vdd_force_vsync = enabled("nvenc_vdd_force_vsync", false);
    }
    return options;
  }

}  // namespace nvprefs
