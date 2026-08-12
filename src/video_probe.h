/**
 * @file src/video_probe.h
 * @brief Platform-independent selection policy for encoder probe displays.
 */
#pragma once

#include <algorithm>
#include <span>
#include <string>

namespace video {

  enum class probe_display_policy_e {
    backend_autoselect,
    exact,
    vdd_compatible
  };

  enum class probe_display_selection_e {
    exact,
    backend_autoselect,
    unavailable
  };

  struct probe_display_selection_t {
    probe_display_selection_e selection;
    std::string display_name;
  };

  /**
   * @brief Select a display for the temporary capture backend used by probing.
   *
   * Exact targets must never collapse to an empty backend selector because an
   * empty selector means "capture any display". VDD probing intentionally keeps
   * that compatibility path: the VDD producer may not be ready, so DDX is used
   * only to validate encoder capability against any capture-ready output.
   */
  inline probe_display_selection_t
  select_encoder_probe_display(
    const std::string &configured_display,
    std::span<const std::string> capture_ready_displays,
    probe_display_policy_e policy) {
    if (policy == probe_display_policy_e::backend_autoselect) {
      return { probe_display_selection_e::backend_autoselect, {} };
    }

    if (!configured_display.empty() &&
        std::ranges::find(capture_ready_displays, configured_display) != capture_ready_displays.end()) {
      return { probe_display_selection_e::exact, configured_display };
    }

    if (policy == probe_display_policy_e::vdd_compatible) {
      return { probe_display_selection_e::backend_autoselect, {} };
    }

    return { probe_display_selection_e::unavailable, {} };
  }

}  // namespace video
