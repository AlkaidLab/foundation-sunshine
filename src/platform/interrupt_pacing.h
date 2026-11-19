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
 *    more than two pads are active so the replies of all of them together stay
 *    within a budget (every reply costs the same, so CPU would otherwise grow
 *    with the pad count).
 *  - A button changed: that state is answered by a reply of its own after the
 *    hard 1 ms floor, whatever the stream floor is; two changes are never
 *    merged into one reply.
 *  - The Windows HID class keeps the last 32 reports of a reader and drops the
 *    oldest ones.  The fixed 4 ms pacing of the heartbeat made those 32 reports
 *    reach back 128 ms; early stream replies would shorten that and a reader
 *    that stalls for 80 ms or more could miss a short press.  What a reader
 *    must still find is a report that shows the button down: while the button
 *    is held every report shows it, so only the release puts a press at risk.
 *    The last report before the release is the one to keep.  After the reply
 *    to a release the lane therefore answers at most once per protect_interval
 *    for protect_window on a 4.5 ms grid (the heartbeat keeps that grid too):
 *    the 32nd reply after the last report that showed the button down, the one
 *    that pushes it out of the queue, comes 139 ms after the release, longer
 *    than the 124 ms of the fixed pacing; a button pressed in the meantime
 *    takes a slot and shortens that by one interval.  Releases less than 32 ms apart (a burst of
 *    presses) keep the fixed pacing's 4 ms and 124 ms: the earlier presses of
 *    such a burst are out of the queue after a stall whatever the pacing is.
 *    A press needs no protection of its own, and stream data keeps its fast
 *    path the rest of the time.
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
    edge,  ///< a button went down: answered by a reply of its own
    release  ///< a button went up: answered by a reply of its own, and the last report that showed it down is protected
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
      unsigned budget_per_second { 1500 };
      /** Up to this many active pads each keep the hard floor; the budget only divides among more pads. */
      unsigned full_rate_pads { 2 };
      /** After the reply to a button release, for this long ... (31 replies at the protect interval) */
      duration protect_window { std::chrono::milliseconds(140) };
      /** ... at most one reply per this interval: the last report that showed the button down then stays in the queue for 139 ms. */
      duration protect_interval { std::chrono::microseconds(4500) };
      /** A release this soon after the previous one belongs to a burst of presses (faster than 30 per second). The
       *  earlier presses of such a burst are gone from the queue after a stall anyway, so the protection falls back
       *  to what the fixed 4 ms pacing gave (124 ms). */
      duration burst_gap { std::chrono::milliseconds(32) };
      duration burst_window { std::chrono::milliseconds(124) };
      duration burst_interval { std::chrono::milliseconds(4) };
    };

    struct taken_t {
      Item item;
      bool edge;  ///< the reply belongs to a button change
    };

    /** Most button changes that wait for their own reply; later ones fall back to stream pacing. */
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
      if (change == report_change::stream) {
        dirty_ = true;
      }
      else if (edges_.size() < kMaxPendingEdges) {
        edges_.push_back(change == report_change::release);
      }
      else {
        /* No room for another reply of its own: the stream carries the state, and a release owes its protection to the next reply. */
        dirty_ = true;
        if (change == report_change::release) guard_owed_ = true;
      }
    }

    /** When the oldest held reply becomes due; nullopt when none is held. @p pads is the number of active pads. */
    std::optional<time_point>
    next_due(unsigned pads) const {
      if (held_.empty()) return std::nullopt;
      /* No reply ever leaves less than min_interval after the previous one actually left. */
      const auto earliest = last_sent_ + floor();
      if (!edges_.empty()) return earliest;
      /* The heartbeat counts from its place on the grid, a stream reply from the moment the last one left. */
      const auto interval = dirty_ ? stream_floor(pads) : settings_.heartbeat;
      const auto anchor = dirty_ ? last_sent_ : grid_;
      const auto normal = std::max(anchor + interval, earliest);
      /* While the protection lasts the replies follow the grid at the protect interval; it ends at
       * protect_until_ whatever the clock does in between, so the due time never waits for a
       * protection that is over. */
      const auto protected_due = std::max(grid_ + std::max(interval, guard_interval_), earliest);
      if (!guarding()) return normal;
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
      const bool edge = !edges_.empty();
      const bool heartbeat = !edge && !dirty_;
      /* A stream reply inside the protection follows the grid like the heartbeat does. */
      const bool on_grid = heartbeat || (!edge && guarding() && *due < protect_until_);
      taken_t taken { std::move(held_.front().item), edge };
      held_.pop_front();
      /* A reply on the grid that leaves a little late (timers are never exact) does not move the next
       * one: the beat stays on its 4 ms grid instead of drifting by the lateness of every timer, and a
       * long gap (idle, first URB) starts a new grid.  last_sent_ is always the real send time, so
       * the floor between two replies holds whatever the grid says. */
      grid_ = on_grid && now - *due <= std::max(settings_.heartbeat - floor(), duration::zero()) ? *due : now;
      last_sent_ = now;
      if (edge) {
        /* The reply to a release starts the protection of the last report that showed the button down. */
        if (edges_.front()) start_guard(now);
        edges_.pop_front();
      }
      else {
        dirty_ = false;
        if (guard_owed_) {
          guard_owed_ = false;
          start_guard(now);
        }
      }
      return taken;
    }

    std::size_t
    held() const noexcept { return held_.size(); }

    bool
    dirty() const noexcept { return dirty_; }

    unsigned
    pending_edges() const noexcept { return static_cast<unsigned>(edges_.size()); }

    /** Drop everything and forget the clock (session end). */
    void
    reset() {
      held_.clear();
      last_sent_ = time_point {};
      grid_ = time_point {};
      protect_until_ = time_point {};
      last_release_ = time_point {};
      guard_interval_ = settings_.protect_interval;
      edges_.clear();
      guard_owed_ = false;
      dirty_ = false;
    }

    /**
     * A lower rate than the hard floor allows, asked for by the user (ds5.compat.inputReportRate): no reply, of any kind, follows the
     * previous one sooner than @p floor.  At the heartbeat (4 ms) or longer the lane answers like the fixed 4 ms pacing, button changes
     * included, and the protection of a press is idle (32 replies then span at least 128 ms), though still recorded.  Zero removes the floor.
     */
    void
    set_floor(duration floor) noexcept { extra_floor_ = floor; }

    /** The stream floor for @p pads active pads. */
    duration
    stream_floor(unsigned pads) const {
      if (pads <= settings_.full_rate_pads) return floor();
      const auto shared = std::chrono::duration_cast<duration>(std::chrono::duration<double>(static_cast<double>(std::max(pads, 1u)) / std::max(settings_.budget_per_second, 1u)));
      /* never slower than the heartbeat: a changed report must not wait longer than an unchanged one */
      return std::min(std::max(floor(), shared), std::max(settings_.heartbeat, floor()));
    }

  private:
    /**
     * Whether a release's protection works: at the heartbeat (4 ms) or slower, every reply is already a heartbeat apart and 32 replies span at
     * least 128 ms, so a press needs none.  The protection of a release is still recorded then, so a faster rate chosen right after it finds
     * the rest of it.
     */
    bool
    guarding() const noexcept { return floor() < settings_.heartbeat; }

    /** The hard floor between two replies: the settled one, or the user's lower rate when that is slower. */
    duration
    floor() const noexcept { return std::max(settings_.min_interval, extra_floor_); }

    void
    start_guard(time_point now) {
      const bool burst = last_release_ != time_point {} && now - last_release_ < settings_.burst_gap;
      last_release_ = now;
      guard_interval_ = burst ? settings_.burst_interval : settings_.protect_interval;
      protect_until_ = now + (burst ? settings_.burst_window : settings_.protect_window);
    }

    struct entry_t {
      std::uint32_t sequence;
      Item item;
    };

    std::deque<entry_t> held_;
    config settings_ {};
    time_point last_sent_ {};  ///< when the last reply really left
    time_point grid_ {};  ///< where the heartbeat grid stands: last_sent_, or a little before it after a late timer
    time_point protect_until_ {};
    time_point last_release_ {};  ///< when the previous release was answered
    duration guard_interval_ { std::chrono::microseconds(4500) };  ///< the protect interval of the protection that runs
    duration extra_floor_ {};
    std::deque<bool> edges_;  ///< one entry per button change waiting for its own reply: true for a release
    bool guard_owed_ { false };  ///< a release found no room of its own; its protection starts with the next reply
    bool dirty_ { false };
  };
}  // namespace platf
