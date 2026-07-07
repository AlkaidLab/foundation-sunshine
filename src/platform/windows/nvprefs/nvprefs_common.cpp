/**
 * @file src/platform/windows/nvprefs/nvprefs_common.cpp
 * @brief Definitions for common nvidia preferences.
 */
// local includes
#include "nvprefs_common.h"

#include <optional>

#ifdef SUNSHINE_NVPREFS_STANDALONE
  #include <iostream>
#else
  #include "src/logging.h"

// Read user override preferences from global Sunshine config.
  #include "src/config.h"
#endif

namespace {

  std::optional<nvprefs::nvprefs_options> options_override;

}  // namespace

namespace nvprefs {

  void
  info_message(const std::wstring &message) {
#ifdef SUNSHINE_NVPREFS_STANDALONE
    std::wcerr << L"nvprefs: " << message << L'\n';
#else
    BOOST_LOG(info) << "nvprefs: " << message;
#endif
  }

  void
  info_message(const std::string &message) {
#ifdef SUNSHINE_NVPREFS_STANDALONE
    std::cerr << "nvprefs: " << message << '\n';
#else
    BOOST_LOG(info) << "nvprefs: " << message;
#endif
  }

  void
  error_message(const std::wstring &message) {
#ifdef SUNSHINE_NVPREFS_STANDALONE
    std::wcerr << L"nvprefs: " << message << L'\n';
#else
    BOOST_LOG(error) << "nvprefs: " << message;
#endif
  }

  void
  error_message(const std::string &message) {
#ifdef SUNSHINE_NVPREFS_STANDALONE
    std::cerr << "nvprefs: " << message << '\n';
#else
    BOOST_LOG(error) << "nvprefs: " << message;
#endif
  }

  void
  set_nvprefs_options(nvprefs_options options) {
    options_override = options;
  }

  nvprefs_options
  get_nvprefs_options() {
    if (options_override) {
      return *options_override;
    }

    nvprefs_options options;
#ifndef SUNSHINE_NVPREFS_STANDALONE
    options.opengl_vulkan_on_dxgi = config::video.nv_opengl_vulkan_on_dxgi;
    options.sunshine_high_power_mode = config::video.nv_sunshine_high_power_mode;
#endif
    return options;
  }

}  // namespace nvprefs
