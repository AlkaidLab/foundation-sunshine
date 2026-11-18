/**
 * @file src/platform/interrupt_pacing.h
 * @brief When a virtual device may answer its interrupt IN URBs.
 *
 * A real HID device answers an interrupt IN request as soon as it has a new
 * report, at most once per endpoint period, and otherwise keeps a steady
 * heartbeat.  A new report is answered at once, but never sooner than
 * min_interval after the previous reply (a guard against a flood); with
 * nothing new the next reply follows heartbeat after the previous one, so
 * sequence numbers and timestamps keep advancing.
 *
 * The host may keep several URBs outstanding.  The time a held URB becomes due
 * is derived from the one thing that matters, the moment the last reply left;
 * no per-URB schedule is accumulated.  An earlier version stamped each URB
 * heartbeat after the previous URB's stamp: replies that new data pulled
 * forward left those stamps ahead of the clock, and after a burst of events
 * the heartbeat stayed silent for as long as the burst had lasted.
 *
 * Pure bookkeeping on time points, so it is tested without a clock or a timer.
 */
#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <utility>

namespace platf {
  template <class Item, class Clock = std::chrono::steady_clock>
  class interrupt_lane {
  public:
    using time_point = typename Clock::time_point;
    using duration = typename Clock::duration;

    interrupt_lane() = default;
    interrupt_lane(duration min_interval, duration heartbeat):
        min_interval_(min_interval), heartbeat_(heartbeat) {}

    /** Hold the reply to an URB (oldest first). */
    void
    hold(std::uint32_t sequence, Item item) {
      held_.push_back({ sequence, std::move(item) });
    }

    /** Remove a held reply; false when it is not held (already taken). */
    bool
    cancel(std::uint32_t sequence) {
      const auto found = std::find_if(held_.begin(), held_.end(), [sequence](const auto &entry) { return entry.sequence == sequence; });
      if (found == held_.end()) return false;
      held_.erase(found);
      return true;
    }

    /** A report changed: the next reply may leave after min_interval instead of heartbeat. */
    void
    new_data() { dirty_ = true; }

    /** When the oldest held reply becomes due, or nullopt when none is held. */
    std::optional<time_point>
    next_due() const {
      if (held_.empty()) return std::nullopt;
      return last_sent_ + (dirty_ ? min_interval_ : heartbeat_);
    }

    /**
     * The oldest held reply when it is due at @p now.  Taking it starts the
     * next interval: every reply still held waits for it, whatever moved the
     * one before.  The caller builds the report after this call, so it carries
     * everything known up to then.
     */
    std::optional<Item>
    take_if_due(time_point now) {
      const auto due = next_due();
      if (!due || *due > now) return std::nullopt;
      auto item = std::move(held_.front().item);
      held_.pop_front();
      last_sent_ = now;
      dirty_ = false;
      return item;
    }

    std::size_t
    held() const noexcept { return held_.size(); }

    bool
    dirty() const noexcept { return dirty_; }

    /** Drop everything and forget the clock (session end). */
    void
    reset() {
      held_.clear();
      last_sent_ = time_point {};
      dirty_ = false;
    }

  private:
    struct entry_t {
      std::uint32_t sequence;
      Item item;
    };

    std::deque<entry_t> held_;
    duration min_interval_ { std::chrono::milliseconds(1) };
    duration heartbeat_ { std::chrono::milliseconds(4) };
    time_point last_sent_ {};
    bool dirty_ { false };
  };
}  // namespace platf
