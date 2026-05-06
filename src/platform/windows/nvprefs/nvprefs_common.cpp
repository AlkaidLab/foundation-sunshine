/**
 * @file src/platform/windows/nvprefs/nvprefs_common.cpp
 * @brief Definitions for common nvidia preferences.
 */
// local includes
#include "nvprefs_common.h"
#include "src/logging.h"

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
    options.opengl_vulkan_on_dxgi = config::video.nv_opengl_vulkan_on_dxgi;
    options.sunshine_high_power_mode = config::video.nv_sunshine_high_power_mode;
    options.nv_optimize_game = config::video.nv_optimize_game;
    options.nv_force_vsync = config::video.nv_force_vsync;
    options.nv_lock_frame_rate = config::video.nv_lock_frame_rate;
    options.nv_frl_fps_offset = config::video.nv_frl_fps_offset;
    options.nv_frl_fps_override = config::video.nv_frl_fps_override;
    options.nv_prefer_max_performance = config::video.nv_prefer_max_performance;
    options.nv_low_latency_mode = config::video.nv_low_latency_mode;
    options.nv_apply_to_base_profile = config::video.nv_apply_to_base_profile;
    return options;
  }

}  // namespace nvprefs
