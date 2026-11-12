/**
 * @file src/ds5/output_fields.h
 * @brief The application-authored fields observed in a DualSense output report.
 *
 * The byte meanings follow source key ds5-linux-hid-playstation-20261031. This is a diagnostic
 * snapshot only: it does not alter the report or decide which tuning policy owns a field.
 */
#pragma once

#include <array>
#include <cstdint>

namespace ds5::output_fields {
  struct snapshot_t {
    std::array<std::uint8_t, 3> rgb {};
    std::array<std::uint8_t, 5> left_trigger {};
    std::array<std::uint8_t, 5> right_trigger {};
    std::uint8_t player_led {};

    friend bool
    operator==(const snapshot_t &, const snapshot_t &) = default;
  };

  template <typename Feedback>
  snapshot_t
  from_feedback(const Feedback &value) {
    snapshot_t result;
    result.rgb = { value.lightbar_rgb[0], value.lightbar_rgb[1], value.lightbar_rgb[2] };
    result.left_trigger = { static_cast<std::uint8_t>(value.left_trigger.mode), value.left_trigger.start,
      value.left_trigger.end, value.left_trigger.strength, value.left_trigger.frequency };
    result.right_trigger = { static_cast<std::uint8_t>(value.right_trigger.mode), value.right_trigger.start,
      value.right_trigger.end, value.right_trigger.strength, value.right_trigger.frequency };
    result.player_led = value.player_led_mask;
    return result;
  }
}  // namespace ds5::output_fields
