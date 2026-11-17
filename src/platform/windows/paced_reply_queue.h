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
 */
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
  #define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace platf {
  class paced_reply_queue final {
  public:
    using clock = std::chrono::steady_clock;
    using send_t = std::function<bool(std::vector<std::uint8_t>)>;
    using make_wire_t = std::function<std::vector<std::uint8_t>()>;

    enum class lane : std::size_t {
      iso_out,
      iso_in,
      interrupt_in,
      count_
    };

    static constexpr auto kIsoPacketPeriod = std::chrono::milliseconds(1);
    static constexpr auto kInterruptPeriod = std::chrono::milliseconds(4);

    explicit paced_reply_queue(send_t send):
        send_(std::move(send)) {
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
     * Hold an interrupt reply for one endpoint period after the previous one.
     * The payload is built when the reply is due so it carries the latest state.
     */
    void
    schedule_interrupt(lane which, std::uint32_t sequence, make_wire_t make_wire) {
      std::lock_guard lock(mutex_);
      auto &next = next_free_[static_cast<std::size_t>(which)];
      const auto due = std::max(clock::now(), next);
      next = due + kInterruptPeriod;
      insert_locked(due, sequence, std::move(make_wire));
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
      return false;
    }

    /** Drop everything and forget the endpoint clocks (session end). */
    void
    clear() {
      std::lock_guard lock(mutex_);
      items_.clear();
      next_free_.fill(clock::time_point {});
    }

    std::size_t
    held() const {
      std::lock_guard lock(mutex_);
      return items_.size();
    }

  private:
    struct item_t {
      std::uint32_t sequence;
      make_wire_t make_wire;
    };

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
          if (items_.empty())
            CancelWaitableTimer(timer_);
          else
            arm_timer(items_.begin()->first - clock::now());
        }
        WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, INFINITE);
      }
    }

    send_t send_;
    mutable std::mutex mutex_;
    std::multimap<clock::time_point, item_t> items_;
    std::array<clock::time_point, static_cast<std::size_t>(lane::count_)> next_free_ {};
    bool stop_ { false };
    bool high_resolution_ { false };
    HANDLE wake_ { nullptr };
    HANDLE timer_ { nullptr };
    std::thread worker_;
  };
}  // namespace platf
