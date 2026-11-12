/**
 * @file src/ds5/sensor_clock.h
 * @brief An evenly paced sensor timestamp for motion samples that arrive unevenly.
 *
 * The pad stamps its samples in its own clock. The wire carries no timestamp, so the host sees the
 * arrival time, and over Bluetooth (about 128 samples per second, jittered by the radio and the
 * network) arrivals bunch up and stretch out. A game that integrates rotation as rate times the
 * difference of two report timestamps then bends the angle wherever the rate changes while an
 * arrival is late. The samples themselves are evenly spaced at the pad, so the host stamps them
 * on an even grid instead: the spacing is the average spacing of the last 64 samples, the stamp
 * never lies after the arrival (a sample cannot be older than the time it arrives... it is never
 * newer), and never more than max_lag (80 ms, longer than a stall the radio makes) behind it, so the grid follows a clock that drifts.
 */
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace ds5 {
  class sensor_clock_t {
  public:
    /** Longest the stamp may trail the arrival; jitter beyond it moves the grid. */
    static constexpr std::int64_t max_lag_us = 80000;
    /** A pause this long starts the grid again. */
    static constexpr std::int64_t restart_gap_us = 250000;

    /** @brief The stamp, in microseconds, of a sample that arrived at @p arrival_us. */
    std::int64_t
    stamp(std::int64_t arrival_us) {
      if (count_ == 0 || arrival_us - last_arrival_ > restart_gap_us || arrival_us < last_arrival_) {
        restart(arrival_us);
        return stamp_;
      }
      last_arrival_ = arrival_us;
      history_[next_ % history_.size()] = arrival_us;
      ++next_;
      count_ = std::min<std::size_t>(count_ + 1, history_.size());
      if (count_ >= 2) {
        const auto oldest = history_[(next_ - count_) % history_.size()];
        period_ = static_cast<double>(arrival_us - oldest) / static_cast<double>(count_ - 1);
      }
      // The grid advances by the sample period. How late the arrivals are compared with it is judged by the
      // earliest arrival of the last samples (an arrival can only be later than its sample, never earlier),
      // and the grid is moved a fortieth of the way per sample so that one jittered arrival bends nothing.
      auto candidate = stamp_ + static_cast<std::int64_t>(period_ + 0.5);
      lateness_[lateness_next_ % lateness_.size()] = arrival_us - candidate;
      ++lateness_next_;
      lateness_count_ = std::min<std::size_t>(lateness_count_ + 1, lateness_.size());
      std::int64_t earliest = lateness_[0];
      for (std::size_t index = 1; index < lateness_count_; ++index) earliest = std::min(earliest, lateness_[index]);
      const auto step = std::clamp<std::int64_t>(earliest / 40, -60, 60);
      candidate += step;
      for (std::size_t index = 0; index < lateness_count_; ++index) lateness_[index] -= step;
      candidate = std::min(candidate, arrival_us);
      candidate = std::max(candidate, arrival_us - max_lag_us);
      candidate = std::max(candidate, stamp_ + 1);  // never backwards
      stamp_ = candidate;
      return stamp_;
    }

    void
    reset() { count_ = 0; }

    double
    period_us() const { return period_; }

  private:
    void
    restart(std::int64_t arrival_us) {
      history_.fill(0);
      history_[0] = arrival_us;
      next_ = 1;
      count_ = 1;
      last_arrival_ = arrival_us;
      stamp_ = arrival_us;
      period_ = 8000.0;
      lateness_.fill(0);
      lateness_next_ = 0;
      lateness_count_ = 0;
    }

    std::array<std::int64_t, 64> history_ {};
    std::array<std::int64_t, 64> lateness_ {};
    std::size_t lateness_next_ { 0 };
    std::size_t lateness_count_ { 0 };
    std::size_t next_ { 0 };
    std::size_t count_ { 0 };
    std::int64_t last_arrival_ { 0 };
    std::int64_t stamp_ { 0 };
    double period_ { 8000.0 };
  };
}  // namespace ds5
