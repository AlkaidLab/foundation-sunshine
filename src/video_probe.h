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
    std::span<const std::string> capture_ready_displays) {
    if (std::ranges::find(capture_ready_displays, configured_display) != capture_ready_displays.end()) {
      return configured_display;
    }

    // An empty display preserves backend auto-selection, including adapter
    // filtering, display wake-up retries, and scanning subsequent outputs.
    return {};
  }

}  // namespace video
