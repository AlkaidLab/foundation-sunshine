/**
 * @file src/display_device/vdd_mode_list.h
 * @brief Pure helpers that build a bounded ZakoVDD SETMODES mode list.
 *
 * Measured on ZakoVDD 0.18.1: lists of roughly 100 modes or more make the
 * monitor fail to publish modes (105 modes) or disappear entirely (112
 * modes), and the driver copies commands into a 2048-wchar_t buffer. A list
 * built from many configured resolutions and refresh rates easily exceeds
 * both. The list therefore never exceeds kMaxTotalModes or kMaxCommandChars.
 * Modes are chosen by priority within that budget:
 *
 *   1. the requested mode;
 *   2. configured sizes at the session refresh rate;
 *   3. configured sizes at the other refresh rates (rate by rate, in
 *      configured order), until the budget is full.
 *
 * The chosen modes are emitted in a fixed order that does not depend on the
 * priority: the smallest-area mode first (the driver reports index 0 as the
 * native mode, and Windows scales any source smaller than native in both
 * dimensions), then configured sizes x refresh rates (resolution-major,
 * configured order), then the requested mode if it is not configured.
 * Repeating a request therefore produces a byte-identical command.
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "display_device.h"

namespace display_device::vdd_mode_list {

  /**
   * @brief Maximum SETMODES command length in wchar_t, excluding the terminator.
   * @details The driver copies commands into a 2048-wchar_t buffer.
   */
  inline constexpr std::size_t kMaxCommandChars = 2047;

  /**
   * @brief Hard ceiling for the number of modes in one SETMODES list.
   */
  inline constexpr std::size_t kMaxTotalModes = 96;

  struct limits_t {
    std::size_t max_total_modes = kMaxTotalModes;
    std::size_t max_command_chars = kMaxCommandChars;
  };

  /**
   * @brief One VDD mode: a size at an integer refresh rate.
   */
  struct vdd_mode_t {
    resolution_t resolution;
    unsigned int refresh_hz;
  };

  constexpr bool
  same_resolution(const resolution_t &a, const resolution_t &b) {
    return a.width == b.width && a.height == b.height;
  }

  constexpr bool
  same_mode(const vdd_mode_t &a, const vdd_mode_t &b) {
    return same_resolution(a.resolution, b.resolution) && a.refresh_hz == b.refresh_hz;
  }

  inline bool
  contains_resolution(const std::vector<resolution_t> &resolutions, const resolution_t &resolution) {
    return std::any_of(resolutions.begin(), resolutions.end(), [&](const auto &entry) {
      return same_resolution(entry, resolution);
    });
  }

  inline bool
  contains_mode(const std::vector<vdd_mode_t> &modes, const vdd_mode_t &mode) {
    return std::any_of(modes.begin(), modes.end(), [&](const auto &entry) {
      return same_mode(entry, mode);
    });
  }

  /**
   * @brief Sizes without duplicates, in their original order.
   */
  inline std::vector<resolution_t>
  unique_resolutions(const std::vector<resolution_t> &resolutions) {
    std::vector<resolution_t> result;
    for (const auto &resolution : resolutions) {
      if (!contains_resolution(result, resolution)) {
        result.push_back(resolution);
      }
    }
    return result;
  }

  /**
   * @brief Non-zero refresh rates without duplicates, in their original order.
   */
  inline std::vector<unsigned int>
  unique_refresh_rates(const std::vector<unsigned int> &refresh_rates_hz) {
    std::vector<unsigned int> result;
    for (const auto refresh_hz : refresh_rates_hz) {
      if (refresh_hz > 0 && std::find(result.begin(), result.end(), refresh_hz) == result.end()) {
        result.push_back(refresh_hz);
      }
    }
    return result;
  }

  /**
   * @brief A planned SETMODES list.
   */
  struct mode_plan_t {
    std::vector<vdd_mode_t> modes;  ///< Modes in emission order.
    std::size_t configured_left_out = 0;  ///< Configured size x rate modes that did not fit the budget.
  };

  /**
   * @brief Characters one mode adds to a SETMODES command, including its separator.
   */
  inline std::size_t
  mode_command_chars(const vdd_mode_t &mode) {
    return std::to_wstring(mode.resolution.width).size() + 1 +
           std::to_wstring(mode.resolution.height).size() + 1 +
           std::to_wstring(mode.refresh_hz).size() + 1;
  }

  /**
   * @brief Index of the mode that must lead the list: the smallest-area size.
   * @details ZakoVDD reports the first SETMODES entry as the monitor's
   *          preferred (native) mode. When a source mode is smaller than the
   *          native mode in both dimensions, Windows commits the native target
   *          timing and scales the desktop onto it, so the VDD producer runs
   *          at the native size instead of the requested one. Leading with the
   *          smallest-area size means no listed size can be smaller in both
   *          dimensions. Ties on area prefer the narrower, then the shorter
   *          size; within that size the session refresh rate wins, otherwise
   *          the first entry in list order.
   */
  inline std::size_t
  smallest_mode_index(const std::vector<vdd_mode_t> &modes, unsigned int session_refresh_hz) {
    std::size_t best = 0;
    const auto area = [](const vdd_mode_t &mode) {
      return static_cast<unsigned long long>(mode.resolution.width) * mode.resolution.height;
    };
    for (std::size_t index = 1; index < modes.size(); ++index) {
      const auto &candidate = modes[index];
      const auto &current = modes[best];
      if (area(candidate) != area(current)) {
        if (area(candidate) < area(current)) {
          best = index;
        }
        continue;
      }
      if (candidate.resolution.width != current.resolution.width) {
        if (candidate.resolution.width < current.resolution.width) {
          best = index;
        }
        continue;
      }
      if (candidate.resolution.height != current.resolution.height) {
        if (candidate.resolution.height < current.resolution.height) {
          best = index;
        }
        continue;
      }
      // Same size: only the session refresh rate displaces the earlier entry.
      if (candidate.refresh_hz == session_refresh_hz && current.refresh_hz != session_refresh_hz) {
        best = index;
      }
    }
    return best;
  }

  /**
   * @brief Move the smallest-area mode to the front; the rest keeps its order.
   */
  inline void
  move_smallest_mode_first(std::vector<vdd_mode_t> &modes, unsigned int session_refresh_hz) {
    if (modes.size() < 2) {
      return;
    }
    const auto index = smallest_mode_index(modes, session_refresh_hz);
    std::rotate(modes.begin(), modes.begin() + static_cast<std::ptrdiff_t>(index), modes.begin() + static_cast<std::ptrdiff_t>(index) + 1);
  }

  /**
   * @brief Choose and order the modes of a SETMODES list (see the file comment).
   * @param configured Configured sizes in configured order.
   * @param refresh_rates_hz Configured refresh rates in configured order.
   * @param session_refresh_hz Refresh rate of the session; configured sizes
   *        at this rate come first among configured modes.
   * @param requested Mode that must be in the list, or nullptr.
   */
  inline mode_plan_t
  plan_modes(const std::vector<resolution_t> &configured,
    const std::vector<unsigned int> &refresh_rates_hz,
    unsigned int session_refresh_hz,
    const vdd_mode_t *requested,
    const limits_t &limits = {}) {
    const auto sizes = unique_resolutions(configured);
    auto rates = unique_refresh_rates(refresh_rates_hz);
    if (session_refresh_hz > 0 && std::find(rates.begin(), rates.end(), session_refresh_hz) == rates.end()) {
      rates.push_back(session_refresh_hz);
    }

    // Select by priority within the mode and command-length budgets.
    std::vector<vdd_mode_t> selected;
    std::size_t chars = std::wstring_view { L"SETMODES " }.size();
    bool budget_full = false;
    const auto select = [&](const vdd_mode_t &mode) {
      if (budget_full || mode.refresh_hz == 0 || contains_mode(selected, mode)) {
        return;
      }
      // The first mode needs no separator; mode_command_chars() counts one.
      const auto added_chars = mode_command_chars(mode) - (selected.empty() ? 1 : 0);
      if (selected.size() >= limits.max_total_modes || chars + added_chars > limits.max_command_chars) {
        budget_full = true;
        return;
      }
      selected.push_back(mode);
      chars += added_chars;
    };

    if (requested) {
      select(*requested);
    }
    if (session_refresh_hz > 0) {
      for (const auto &resolution : sizes) {
        select({ resolution, session_refresh_hz });
      }
    }
    for (const auto refresh_hz : rates) {
      if (refresh_hz == session_refresh_hz) {
        continue;
      }
      for (const auto &resolution : sizes) {
        select({ resolution, refresh_hz });
      }
    }

    // Emit in the fixed order: configured product, then the requested mode
    // if it is not configured.
    mode_plan_t plan;
    plan.modes.reserve(selected.size());
    std::size_t configured_selected = 0;
    for (const auto &resolution : sizes) {
      for (const auto refresh_hz : rates) {
        const vdd_mode_t mode { resolution, refresh_hz };
        if (contains_mode(selected, mode)) {
          plan.modes.push_back(mode);
          ++configured_selected;
        }
      }
    }
    if (requested && contains_mode(selected, *requested) && !contains_mode(plan.modes, *requested)) {
      plan.modes.push_back(*requested);
    }
    plan.configured_left_out = sizes.size() * rates.size() - configured_selected;
    move_smallest_mode_first(plan.modes, session_refresh_hz);
    return plan;
  }

  /**
   * @brief Format a SETMODES command ("SETMODES WxHxR,...").
   * @return An empty string when the list is empty.
   */
  inline std::wstring
  format_setmodes_command(const std::vector<vdd_mode_t> &modes) {
    if (modes.empty()) {
      return {};
    }

    std::wstring command = L"SETMODES ";
    bool first = true;
    for (const auto &mode : modes) {
      if (!first) {
        command += L',';
      }
      first = false;
      command += std::to_wstring(mode.resolution.width);
      command += L'x';
      command += std::to_wstring(mode.resolution.height);
      command += L'x';
      command += std::to_wstring(mode.refresh_hz);
    }
    return command;
  }

}  // namespace display_device::vdd_mode_list
