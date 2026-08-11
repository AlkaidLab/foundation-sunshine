/**
 * @file src/video_probe.h
 * @brief Platform-independent selection policy for encoder probe displays.
 */
#pragma once

#include <algorithm>
#include <span>
#include <string>

namespace video {

  inline std::string
  select_encoder_probe_display(
    const std::string &configured_display,
    std::span<const std::string> capture_ready_displays,
    const std::string &configured_adapter) {
    if (std::ranges::find(capture_ready_displays, configured_display) != capture_ready_displays.end()) {
      return configured_display;
    }

    // An empty display lets the backend select the first capture-ready output
    // on the configured adapter. Choosing the global list's first output here
    // could force reset_display() onto a different GPU.
    if (!configured_adapter.empty()) {
      return {};
    }

    return capture_ready_displays.empty() ? std::string {} : capture_ready_displays.front();
  }

}  // namespace video
