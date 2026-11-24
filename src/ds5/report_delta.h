/**
 * @file src/ds5/report_delta.h
 * @brief What changed between two DualSense input reports (USB report 0x01, 64 bytes).
 *
 * The host answers a changed report at once and keeps a heartbeat for an unchanged one, so it
 * must tell a change from a repeat.  Two fields move by themselves and say nothing about the
 * state: the sequence counter (byte 7) and the sensor timestamp (bytes 28 to 31).  The button
 * bytes (8 to 10: hat, face and shoulder buttons, PS, touchpad click, mute) are told apart
 * from the continuous values (sticks, triggers, motion, touch position, battery, jacks)
 * because a button change is answered by a report of its own.  A button that went up is told
 * apart from one that went down: only the release ends a press, and the host protects the last
 * report that still showed the button down (see interrupt_pacing.h).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace ds5 {
  enum class report_delta {
    none,  ///< the same state, apart from the fields that move by themselves
    stream,  ///< a continuous value changed
    buttons,  ///< a button went down (a continuous value may have changed as well)
    release  ///< at least one button went up (or the hat left its direction)
  };

  /** True when the button byte @p index lost a bit it had: a button went up, or the hat left its direction. */
  inline bool
  button_byte_released(std::size_t index, std::uint8_t before, std::uint8_t after) {
    if (index == 8) {
      /* low nibble: hat (0 to 7 a direction, 8 neutral); high nibble: square, cross, circle, triangle */
      const auto hat_before = static_cast<std::uint8_t>(before & 0x0f);
      const auto hat_after = static_cast<std::uint8_t>(after & 0x0f);
      if (hat_before < 8 && hat_before != hat_after) return true;
      return (before & ~after & 0xf0) != 0;
    }
    return (before & ~after) != 0;
  }

  inline report_delta
  input_report_delta(std::span<const std::uint8_t> before, std::span<const std::uint8_t> after) {
    const auto size = before.size() < after.size() ? before.size() : after.size();
    bool stream = false;
    bool buttons = false;
    bool released = false;
    for (std::size_t i = 1; i < size; ++i) {
      if (i == 7 || (i >= 28 && i <= 31)) continue;
      if (before[i] == after[i]) continue;
      if (i >= 8 && i <= 10) {
        buttons = true;
        released = released || button_byte_released(i, before[i], after[i]);
        continue;
      }
      stream = true;
    }
    if (released) return report_delta::release;
    if (buttons) return report_delta::buttons;
    return stream ? report_delta::stream : report_delta::none;
  }
}  // namespace ds5
