/**
 * @file src/platform/windows/paced_reply_queue.h
 * @brief Wall-clock pacing for the USB/IP replies of virtual devices.
 *
 * A real device completes an isochronous transfer when its frames have gone
 * by on the bus: the packets of one URB occupy one packet period each.  The
 * Windows audio class driver trusts that clock for its stream position, so
 * replying at once makes the stream run many times faster than real time.
 * Every reply therefore waits in this queue until its slot has ended.  A
 * single worker owns a high resolution waitable timer; the USB/IP session
 * thread only inserts and cancels, it never sleeps.
 *
 * The device descriptors fix the periods: the isochronous endpoints of
 * the virtual devices use bInterval 4 at high speed (eight microframes, one
 * millisecond per packet) and the DualSense interrupt IN endpoint uses
 * bInterval 6 (32 microframes, 4 ms).
 *
 * The interrupt IN endpoint is not a stream.  Its reply leaves as soon as a
 * new report exists (notify_interrupt_data); with nothing new it keeps the 4 ms
 * heartbeat.  The rules, including the pad-count budget and the protection of
 * button replies against the 32-report HID queue, are in interrupt_pacing.h.
 */
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
  #define NOMINMAX
#endif
#include <windows.h>

#include "src/platform/interrupt_pacing.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace platf {
  class paced_reply_queue final {
  public:
    using clock = std::chrono::steady_clock;
    using send_t = std::function<bool(std::vector<std::uint8_t>)>;
    using make_wire_t = std::function<std::vector<std::uint8_t>()>;
    /** Builds an interrupt reply; the argument tells whether it answers a button change. */
    using interrupt_wire_t = std::function<std::vector<std::uint8_t>(bool)>;

    enum class lane : std::size_t {
      iso_out,
      iso_in,
      interrupt_in,
      count_
    };

    static constexpr auto kIsoPacketPeriod = std::chrono::milliseconds(1);
    static constexpr auto kInterruptPeriod = std::chrono::milliseconds(4);

    using interrupt_config = interrupt_lane<interrupt_wire_t, clock>::config;

    explicit paced_reply_queue(send_t send):
        paced_reply_queue(std::move(send), interrupt_config {}, {}) {}

    /**
     * @param active_pads How many pads share the reply budget (the stream floor of
     *                    every pad grows with it); null counts as one.
     */
    paced_reply_queue(send_t send, interrupt_config config, std::function<unsigned()> active_pads = {}):
        send_(std::move(send)),
        active_pads_(std::move(active_pads)) {
      for (auto &lane_state : interrupt_) lane_state = interrupt_lane_t(config);
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
  #define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
      wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
      high_resolution_ = timer_ != nullptr;
      if (!timer_) timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
      worker_ = std::thread([this] { run(); });
    }

    ~paced_reply_queue() {
      {
        std::lock_guard lock(mutex_);
        stop_ = true;
      }
      SetEvent(wake_);
      if (worker_.joinable()) worker_.join();
      if (timer_) CloseHandle(timer_);
      if (wake_) CloseHandle(wake_);
    }

    paced_reply_queue(const paced_reply_queue &) = delete;
    paced_reply_queue &
    operator=(const paced_reply_queue &) = delete;

    /** True when the timer has sub-millisecond resolution (Windows 10 1803+). */
    bool
    high_resolution() const noexcept { return high_resolution_; }

    /**
     * Hold an isochronous reply until the end of its slot.  The slot is
     * `packets` periods long and starts when the previous URB of the same
     * endpoint ended, or now when the endpoint ran dry.
     */
    void
    schedule_iso(lane which, std::uint32_t sequence, std::uint32_t packets, std::vector<std::uint8_t> wire) {
      std::lock_guard lock(mutex_);
      auto &next = next_free_[static_cast<std::size_t>(which)];
      const auto start = std::max(clock::now(), next);
      const auto end = start + kIsoPacketPeriod * packets;
      next = end;
      insert_locked(end, sequence, [wire = std::move(wire)]() mutable { return std::move(wire); });
    }

    /**
     * Like schedule_iso(), but the reply is built when its slot has ended, so
     * a device that streams live data (a microphone) delivers the freshest
     * packets the bus clock allows instead of data fixed at submit time.
     */
    void
    schedule_iso_lazy(lane which, std::uint32_t sequence, std::uint32_t packets,
      std::chrono::nanoseconds packet_period, make_wire_t make_wire) {
      std::lock_guard lock(mutex_);
      auto &next = next_free_[static_cast<std::size_t>(which)];
      const auto start = std::max(clock::now(), next);
      const auto end = start + packet_period * packets;
      next = end;
      insert_locked(end, sequence, std::move(make_wire));
    }

    /**
     * Hold an interrupt reply: due at once when new data is waiting (at least
     * min_interval after the previous reply), one heartbeat after the previous
     * reply otherwise.  The payload is built when the reply is due so it
     * carries the latest state.
     */
    void
    schedule_interrupt(lane which, std::uint32_t sequence, interrupt_wire_t make_wire) {
      std::lock_guard lock(mutex_);
      interrupt_[static_cast<std::size_t>(which)].hold(sequence, std::move(make_wire));
      SetEvent(wake_);
    }

    /**
     * A new report exists: the held interrupt reply may leave after the
     * minimum interval instead of the heartbeat, or, when no URB is held, the
     * next one does.  Never call it while holding a lock the reply builder
     * takes: the worker runs the builder under this queue's lock.
     */
    void
    notify_interrupt_data(lane which, report_change change = report_change::stream) {
      std::lock_guard lock(mutex_);
      auto &lane_state = interrupt_[static_cast<std::size_t>(which)];
      /* The worker only needs to wake when the due time of the held reply moves up: from the
       * heartbeat to the stream floor, or from either to the button floor.  Further data of the
       * same kind leaves it where it is. */
      const bool had_edge = lane_state.pending_edges() != 0;
      const bool was_dirty = lane_state.dirty();
      lane_state.new_data(change);
      const bool earlier = change == report_change::edge ? !had_edge : !had_edge && !was_dirty;
      if (earlier && lane_state.held() != 0) SetEvent(wake_);
    }

    /** Remove a held reply.  Returns false when it was already sent. */
    bool
    cancel(std::uint32_t sequence) {
      std::lock_guard lock(mutex_);
      for (auto item = items_.begin(); item != items_.end(); ++item) {
        if (item->second.sequence == sequence) {
          items_.erase(item);
          return true;
        }
      }
      for (auto &lane_state : interrupt_) {
        if (lane_state.cancel(sequence)) return true;
      }
      return false;
    }

    /** Drop everything and forget the endpoint clocks (session end). */
    void
    clear() {
      std::lock_guard lock(mutex_);
      items_.clear();
      next_free_.fill(clock::time_point {});
      for (auto &lane_state : interrupt_) lane_state.reset();
    }

    std::size_t
    held() const {
      std::lock_guard lock(mutex_);
      std::size_t count = items_.size();
      for (const auto &lane_state : interrupt_) count += lane_state.held();
      return count;
    }

  private:
    struct item_t {
      std::uint32_t sequence;
      make_wire_t make_wire;
    };

    using interrupt_lane_t = interrupt_lane<interrupt_wire_t, clock>;

    unsigned
    pads() const { return active_pads_ ? std::max(1u, active_pads_()) : 1u; }

    void
    insert_locked(clock::time_point due, std::uint32_t sequence, make_wire_t make_wire) {
      items_.emplace(due, item_t { sequence, std::move(make_wire) });
      SetEvent(wake_);
    }

    void
    arm_timer(clock::duration delay) {
      const auto ticks = std::max<long long>(1, std::chrono::duration_cast<std::chrono::nanoseconds>(delay).count() / 100);
      LARGE_INTEGER due;
      due.QuadPart = -ticks;
      SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE);
    }

    void
    run() {
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
      const std::array<HANDLE, 2> handles { wake_, timer_ };
      for (;;) {
        {
          /* The send happens under the lock so cancel() can tell a reply that
           * is still held from one that is already on the wire. */
          std::lock_guard lock(mutex_);
          if (stop_) return;
          while (!items_.empty() && items_.begin()->first <= clock::now()) {
            auto node = items_.extract(items_.begin());
            send_(node.mapped().make_wire());
          }
          const auto active = pads();
          for (auto &lane_state : interrupt_) {
            /* Taking a reply starts the next interval, so at most one leaves per call. */
            if (auto taken = lane_state.take_if_due(clock::now(), active)) send_(taken->item(taken->edge));
          }
          std::optional<clock::time_point> wake;
          if (!items_.empty()) wake = items_.begin()->first;
          for (const auto &lane_state : interrupt_) {
            if (const auto due = lane_state.next_due(active); due && (!wake || *due < *wake)) wake = *due;
          }
          if (!wake)
            CancelWaitableTimer(timer_);
          else
            arm_timer(*wake - clock::now());
        }
        WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, INFINITE);
      }
    }

    send_t send_;
    std::function<unsigned()> active_pads_;
    mutable std::mutex mutex_;
    std::multimap<clock::time_point, item_t> items_;
    std::array<clock::time_point, static_cast<std::size_t>(lane::count_)> next_free_ {};
    std::array<interrupt_lane_t, static_cast<std::size_t>(lane::count_)> interrupt_;
    bool stop_ { false };
    bool high_resolution_ { false };
    HANDLE wake_ { nullptr };
    HANDLE timer_ { nullptr };
    std::thread worker_;
  };
}  // namespace platf
