/**
 * @file src/platform/interrupt_pacing.h
 * @brief When a virtual device may answer its interrupt IN URBs.
 *
 * A real HID device answers an interrupt IN request as soon as it has a new
 * report, at most once per endpoint period, and otherwise keeps a steady
 * heartbeat.  The rules here:
 *
 *  - Nothing changed: the next reply follows one heartbeat after the previous
 *    one, so sequence numbers and timestamps keep advancing.
 *  - A stream value changed (stick, analog trigger, motion, touch position):
 *    the next reply follows after the stream floor, at least 1 ms, longer when
 *    many pads are active so the replies of all of them together stay within a
 *    budget (every reply costs the same, so CPU would otherwise grow with the
 *    pad count).
 *  - A button changed: that state is answered by a reply of its own after the
 *    hard 1 ms floor, whatever the stream floor is; two changes are never
 *    merged into one reply.
 *  - The Windows HID class keeps the last 32 reports of a reader and drops the
 *    oldest ones.  The fixed 4 ms pacing of the heartbeat made those 32 reports
 *    reach back 128 ms; early stream replies would shorten that and a reader
 *    that stalls for 80 ms or more could miss a short press.  After a button
 *    reply the lane therefore answers at most once per protect_interval for
 *    protect_window (32 replies); at 4.5 ms the 32 slots hold the press, the
 *    reply to its release and 29 more for the full 128 ms of the fixed pacing.
 *    Stream data keeps its fast path the rest of the time.
 *
 * The host may keep several URBs outstanding.  The time a held URB becomes due
 * is derived from the moment the last reply left; no per-URB schedule is
 * accumulated (an earlier version stamped each URB one heartbeat after the
 * previous URB's stamp; replies pulled forward by new data left those stamps
 * ahead of the clock and the heartbeat fell silent for as long as a burst of
 * events had lasted).
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
  /** What a changed report contains. */
  enum class report_change {
    stream,  ///< a continuous value moved: coalesced, paced by the stream floor
    edge  ///< a button went down or up: answered by a reply of its own
  };

  template <class Item, class Clock = std::chrono::steady_clock>
  class interrupt_lane {
  public:
    using time_point = typename Clock::time_point;
    using duration = typename Clock::duration;

    struct config {
      /** Hard floor between two replies. */
      duration min_interval { std::chrono::milliseconds(1) };
      /** Period of the reply when nothing changed. */
      duration heartbeat { std::chrono::milliseconds(4) };
      /** Replies per second all active pads may use together for stream changes. */
      unsigned budget_per_second { 2000 };
      /** After a button reply, for this long ... (32 reports of the HID queue at 4.5 ms) */
      duration protect_window { std::chrono::milliseconds(144) };
      /** ... at most one reply per this interval (a little over the old 4 ms: the release of the press needs a slot too). */
      duration protect_interval { std::chrono::microseconds(4500) };
    };

    struct taken_t {
      Item item;
      bool edge;  ///< the reply belongs to a button change
    };

    /** Most button changes that wait for their own reply; older ones fall back to stream pacing. */
    static constexpr unsigned kMaxPendingEdges = 8;

    interrupt_lane() = default;
    explicit interrupt_lane(config settings):
        settings_(settings) {}

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

    /** A report changed. */
    void
    new_data(report_change change) {
      if (change == report_change::edge && pending_edges_ < kMaxPendingEdges)
        ++pending_edges_;
      else
        dirty_ = true;
    }

    /** When the oldest held reply becomes due; nullopt when none is held. @p pads is the number of active pads. */
    std::optional<time_point>
    next_due(unsigned pads) const {
      if (held_.empty()) return std::nullopt;
      /* No reply ever leaves less than min_interval after the previous one actually left. */
      const auto earliest = last_sent_ + settings_.min_interval;
      if (pending_edges_ != 0) return earliest;
      /* The heartbeat counts from its place on the grid, a stream reply from the moment the last one left. */
      const auto interval = dirty_ ? stream_floor(pads) : settings_.heartbeat;
      const auto anchor = dirty_ ? last_sent_ : grid_;
      const auto normal = std::max(anchor + interval, earliest);
      /* While the protection lasts the interval is longer; it ends at protect_until_ whatever
       * the clock does in between, so the due time never waits for a protection that is over. */
      const auto protected_due = std::max(anchor + std::max(interval, settings_.protect_interval), earliest);
      return std::max(normal, std::min(protected_due, protect_until_));
    }

    /**
     * The oldest held reply when it is due at @p now.  Taking it starts the
     * next interval: every reply still held waits for it, whatever moved the
     * one before.  The caller builds the report after this call, so it carries
     * everything known up to then (a button reply carries the state of the
     * change itself).
     */
    std::optional<taken_t>
    take_if_due(time_point now, unsigned pads) {
      const auto due = next_due(pads);
      if (!due || *due > now) return std::nullopt;
      const bool edge = pending_edges_ != 0;
      const bool heartbeat = !edge && !dirty_;
      taken_t taken { std::move(held_.front().item), edge };
      held_.pop_front();
      /* A heartbeat that leaves a little late (timers are never exact) does not move the next one:
       * the beat stays on its 4 ms grid instead of drifting by the lateness of every timer, and a
       * long gap (idle, first URB) starts a new grid.  last_sent_ is always the real send time, so
       * the floor between two replies holds whatever the grid says. */
      grid_ = heartbeat && now - *due <= settings_.heartbeat - settings_.min_interval ? *due : now;
      last_sent_ = now;
      if (edge) {
        --pending_edges_;
        protect_until_ = now + settings_.protect_window;
      }
      else {
        dirty_ = false;
      }
      return taken;
    }

    std::size_t
    held() const noexcept { return held_.size(); }

    bool
    dirty() const noexcept { return dirty_; }

    unsigned
    pending_edges() const noexcept { return pending_edges_; }

    /** Drop everything and forget the clock (session end). */
    void
    reset() {
      held_.clear();
      last_sent_ = time_point {};
      grid_ = time_point {};
      protect_until_ = time_point {};
      pending_edges_ = 0;
      dirty_ = false;
    }

    /** The stream floor for @p pads active pads. */
    duration
    stream_floor(unsigned pads) const {
      const auto shared = std::chrono::duration_cast<duration>(std::chrono::duration<double>(static_cast<double>(std::max(pads, 1u)) / std::max(settings_.budget_per_second, 1u)));
      return std::max(settings_.min_interval, shared);
    }

  private:
    struct entry_t {
      std::uint32_t sequence;
      Item item;
    };

    std::deque<entry_t> held_;
    config settings_ {};
    time_point last_sent_ {};  ///< when the last reply really left
    time_point grid_ {};  ///< where the heartbeat grid stands: last_sent_, or a little before it after a late timer
    time_point protect_until_ {};
    unsigned pending_edges_ { 0 };
    bool dirty_ { false };
  };
}  // namespace platf
