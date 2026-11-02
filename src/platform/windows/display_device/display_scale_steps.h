#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace display_device::w_utils::dpi_scale {

  // GET/SET_DPI_SCALE use indices into this desktop scale table, relative to
  // the recommended scale. Other DEVICE_SCALE_FACTOR enum values are not steps.
  inline constexpr std::array<int, 12> percent_values {
    100, 125, 150, 175, 200, 225, 250, 300, 350, 400, 450, 500
  };

  struct range_t {
    int recommended_index;
    int current_index;
    int max_index;
  };

  constexpr std::optional<range_t>
  decode_range(std::int32_t min_rel, std::int32_t current_rel, std::int32_t max_rel) {
    // Windows reports 100% as the minimum; widen before negating API values.
    const std::int64_t recommended_index { -static_cast<std::int64_t>(min_rel) };
    const std::int64_t current_index { recommended_index + current_rel };
    const std::int64_t max_index { recommended_index + max_rel };
    if (recommended_index < 0 || current_index < 0 ||
        current_index > max_index || recommended_index > max_index ||
        max_index >= static_cast<std::int64_t>(percent_values.size())) {
      return std::nullopt;
    }
    return range_t {
      static_cast<int>(recommended_index),
      static_cast<int>(current_index),
      static_cast<int>(max_index)
    };
  }

  constexpr std::optional<std::int32_t>
  relative_from_percent(int percent, int recommended_index) {
    if (recommended_index < 0 || recommended_index >= static_cast<int>(percent_values.size())) {
      return std::nullopt;
    }
    for (int i = 0; i < static_cast<int>(percent_values.size()); ++i) {
      if (percent_values[i] == percent) {
        return i - recommended_index;
      }
    }
    return std::nullopt;
  }

}  // namespace display_device::w_utils::dpi_scale
