/**
 * @file src/ds5/rumble_policy.h
 * @brief When a DualSense output report moves the motors, and the log of what games send.
 *
 * The enable bits come from the output report layout of the Linux hid-playstation driver
 * (DS_OUTPUT_VALID_FLAG0_COMPATIBLE_VIBRATION, DS_OUTPUT_VALID_FLAG0_HAPTICS_SELECT,
 * DS_OUTPUT_VALID_FLAG2_COMPATIBLE_VIBRATION2) and from the way SDL's HIDAPI PS5 driver
 * writes them:
 *
 *   flag0 bit 0 or flag2 bit 2   the motor bytes are valid (compatible vibration or improved
 *                                rumble emulation).
 *   flag0 bit 1                  haptics select: the report takes over from audio haptics. SDL
 *                                sends it alone, without motor values, as the first report of a
 *                                rumble (before the values follow), and the Linux driver sets it
 *                                together with the motor bits.
 *   no motor bit                 SDL repeats its whole state in every report and ends a rumble
 *                                with a report that has every enable bit clear.
 *
 * A game's real report is not known in advance, so the policy keeps the pad's motors from
 * sticking on: any report that carries values moves the motors, and a stop is raised whenever a
 * running rumble is ended explicitly (zero values with a motor or select bit, a report without
 * any enable bit) or when the pad goes away.
 */
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <set>

namespace ds5::rumble {
  /** @brief The bytes of an output report body (report id removed) that decide the motors. */
  struct report_t {
    std::uint8_t flag0 {};  ///< body byte 0
    std::uint8_t flag1 {};  ///< body byte 1
    std::uint8_t flag2 {};  ///< body byte 38
    std::uint8_t right {};  ///< body byte 2, the high frequency motor
    std::uint8_t left {};  ///< body byte 3, the low frequency motor
  };

  /** @brief Read the deciding bytes from a 47-byte output report body. */
  inline report_t
  from_body(const std::uint8_t *body) {
    return { body[0], body[1], body[38], body[2], body[3] };
  }

  enum class reason_e : std::uint8_t {
    none,
    motor_bits,  ///< flag0 bit 0 or flag2 bit 2: the values of the report
    haptics_select_values,  ///< flag0 bit 1 without a motor bit but with nonzero values
    haptics_select_stop,  ///< flag0 bit 1, zero values, a rumble was running
    empty_report_stop,  ///< no enable bit at all, a rumble was running
  };

  struct decision_t {
    bool raise { false };
    std::uint8_t left {};
    std::uint8_t right {};
    reason_e reason { reason_e::none };
  };

  inline const char *
  to_string(reason_e reason) {
    switch (reason) {
      case reason_e::motor_bits: return "motor bits";
      case reason_e::haptics_select_values: return "haptics select with values";
      case reason_e::haptics_select_stop: return "haptics select stop";
      case reason_e::empty_report_stop: return "empty report stop";
      default: return "no motor change";
    }
  }

  /** @brief Per pad: whether the motors were last told to run. */
  class tracker_t {
  public:
    decision_t
    decide(const report_t &report) {
      const bool motor_bits = (report.flag0 & 0x01u) != 0u || (report.flag2 & 0x04u) != 0u;
      const bool select = (report.flag0 & 0x02u) != 0u;
      const bool values = (report.left | report.right) != 0u;
      decision_t result;
      if (motor_bits) {
        result = { true, report.left, report.right, reason_e::motor_bits };
        running_ = values;
      }
      else if (select && values) {
        result = { true, report.left, report.right, reason_e::haptics_select_values };
        running_ = true;
      }
      else if (select && running_) {
        result = { true, 0, 0, reason_e::haptics_select_stop };
        running_ = false;
      }
      else if (!select && (report.flag0 | report.flag1 | report.flag2) == 0u && running_) {
        result = { true, 0, 0, reason_e::empty_report_stop };
        running_ = false;
      }
      return result;
    }

    /** @brief The pad goes away: true when a stop has to be raised. */
    bool
    take_stop() {
      const bool was_running = running_;
      running_ = false;
      return was_running;
    }

    bool
    running() const { return running_; }

  private:
    bool running_ { false };
  };

  /**
   * @brief Decides which output reports are worth a log line: one line per distinct
   *        combination of flag0, flag1, flag2 and "motors nonzero", at most @p max_lines per pad
   *        and never closer together than @p min_gap. A combination that was held back by the
   *        gap is offered again with its next report.
   */
  class combination_log_t {
  public:
    using clock = std::chrono::steady_clock;

    explicit combination_log_t(std::size_t max_lines = 48, clock::duration min_gap = std::chrono::milliseconds(100)):
        max_lines_(max_lines), min_gap_(min_gap) {}

    bool
    should_log(const report_t &report, clock::time_point now) {
      const std::uint32_t key = static_cast<std::uint32_t>(report.flag0) | (static_cast<std::uint32_t>(report.flag1) << 8) |
                                (static_cast<std::uint32_t>(report.flag2) << 16) |
                                (((report.left | report.right) != 0u ? 1u : 0u) << 24);
      if (seen_.contains(key) || seen_.size() >= max_lines_) return false;
      if (logged_any_ && now - last_ < min_gap_) return false;
      seen_.insert(key);
      logged_any_ = true;
      last_ = now;
      return true;
    }

    std::size_t
    count() const { return seen_.size(); }

  private:
    std::size_t max_lines_;
    clock::duration min_gap_;
    std::set<std::uint32_t> seen_;
    clock::time_point last_ {};
    bool logged_any_ { false };
  };
}  // namespace ds5::rumble
