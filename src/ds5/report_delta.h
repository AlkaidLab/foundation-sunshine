/**
 * @file src/ds5/report_delta.h
 * @brief What changed between two DualSense input reports (USB report 0x01, 64 bytes).
 *
 * The host answers a changed report at once and keeps a heartbeat for an unchanged one, so it
 * must tell a change from a repeat.  Two fields move by themselves and say nothing about the
 * state: the sequence counter (byte 7) and the sensor timestamp (bytes 28 to 31).  The button
 * bytes (8 to 10: hat, face and shoulder buttons, PS, touchpad click, mute) are told apart
 * from the continuous values (sticks, triggers, motion, touch position, battery, jacks)
 * because a button change is answered by a report of its own.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace ds5 {
  enum class report_delta {
    none,  ///< the same state, apart from the fields that move by themselves
    stream,  ///< a continuous value changed
    buttons  ///< a button changed (a continuous value may have changed as well)
  };

  inline report_delta
  input_report_delta(std::span<const std::uint8_t> before, std::span<const std::uint8_t> after) {
    const auto size = before.size() < after.size() ? before.size() : after.size();
    bool stream = false;
    for (std::size_t i = 1; i < size; ++i) {
      if (i == 7 || (i >= 28 && i <= 31)) continue;
      if (before[i] == after[i]) continue;
      if (i >= 8 && i <= 10) return report_delta::buttons;
      stream = true;
    }
    return stream ? report_delta::stream : report_delta::none;
  }
}  // namespace ds5
