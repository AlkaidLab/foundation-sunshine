/**
 * @file src/platform/interrupt_pacing.h
 * @brief When a virtual device may answer its interrupt IN URB.
 *
 * A real HID device answers an interrupt IN request as soon as it has a new
 * report, at most once per endpoint period, and otherwise keeps a steady
 * heartbeat.  A new report is answered immediately but never sooner than
 * min_interval after the previous reply (a guard against a flood); with
 * nothing new the reply follows after the heartbeat period so sequence
 * numbers and timestamps keep advancing.  Pure arithmetic on time points, so
 * the policy is tested without a clock or a timer.
 */
#pragma once

#include <algorithm>
#include <chrono>

namespace platf {
  template <class Clock = std::chrono::steady_clock>
  class interrupt_pacing {
  public:
    using time_point = typename Clock::time_point;
    using duration = typename Clock::duration;

    interrupt_pacing() = default;
    interrupt_pacing(duration min_interval, duration heartbeat):
        min_interval_(min_interval), heartbeat_(heartbeat) {}

    /** Due time of a reply to an URB that arrived at @p now. */
    time_point
    schedule(time_point now) {
      const auto base = std::max(chain_end_, last_sent_);
      const auto due = std::max(now, base + (dirty_ ? min_interval_ : heartbeat_));
      chain_end_ = due;
      return due;
    }

    /**
     * New report data exists.  @p held_due is the due time of the earliest
     * reply held for this endpoint, or nullopt-like `has_held == false`.
     * Returns the earlier due time when that reply should move up.
     */
    bool
    new_data(time_point now, bool has_held, time_point held_due, time_point &moved_to) {
      dirty_ = true;
      if (!has_held) return false;
      const auto earliest = std::max(now, last_sent_ + min_interval_);
      if (earliest >= held_due) return false;
      moved_to = earliest;
      if (chain_end_ <= held_due) chain_end_ = earliest;
      return true;
    }

    /** A reply left at @p now; it carried everything known so far. */
    void
    sent(time_point now) {
      last_sent_ = now;
      dirty_ = false;
    }

    void
    reset() { *this = interrupt_pacing(min_interval_, heartbeat_); }

    bool
    dirty() const noexcept { return dirty_; }

  private:
    duration min_interval_ { std::chrono::milliseconds(1) };
    duration heartbeat_ { std::chrono::milliseconds(4) };
    time_point chain_end_ {};
    time_point last_sent_ {};
    bool dirty_ { false };
  };
}  // namespace platf
