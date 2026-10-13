/**
 * @file src/streaming/frame_rate_tracker.h
 * @brief Capture-source counters and a common measurement window for frame rates.
 */
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>

namespace perf {
  // Zero means disabled. A new nonzero epoch starts each collection interval.
  extern std::atomic<std::uint64_t> frame_rate_collection_generation;

  inline std::uint64_t frame_rate_collection_epoch() {
    return frame_rate_collection_generation.load(std::memory_order_acquire);
  }

  enum class capture_source_kind_e {
    unavailable,
    dxgi_updates,
    wgc_arrivals,
    vdd_published,
  };

  // One counter per display lifetime. Source threads only increment atomics;
  // they never wait for the performance API or the RTSS monitor.
  struct capture_source_counter_t {
    std::atomic<capture_source_kind_e> kind { capture_source_kind_e::unavailable };
    std::atomic<std::uint64_t> frames { 0 };

    void record(std::uint64_t count = 1) {
      if (!frame_rate_collection_epoch()) return;
      frames.fetch_add(count, std::memory_order_relaxed);
    }

    // Only the VDD capture thread calls this. Count skipped publications too,
    // without treating the driver's pre-stream counter as newly arrived frames.
    void observe_sequence(std::uint64_t sequence) {
      const auto generation = frame_rate_collection_epoch();
      if (!generation) return;
      if (generation != last_generation) {
        last_sequence = sequence;
        last_generation = generation;
        return;
      }
      if (last_sequence && sequence >= *last_sequence) {
        record(sequence - *last_sequence);
      }
      last_sequence = sequence;
    }

  private:
    std::optional<std::uint64_t> last_sequence;
    std::uint64_t last_generation = 0;
  };

  // Source, capture delivery, encode submission, encoded output, repeat submission.
  class frame_rate_window_t {
  public:
    using clock_t = std::chrono::steady_clock;
    using counts_t = std::array<std::uint64_t, 5>;
    using rates_t = std::array<double, 5>;

    void reset(clock_t::time_point now, const counts_t &counts) {
      started_at = now;
      baseline = counts;
      rates.reset();
      window_seconds = 0.0;
    }

    const std::optional<rates_t> &sample(clock_t::time_point now, const counts_t &counts) {
      for (std::size_t i = 0; i < counts.size(); ++i) {
        if (counts[i] < baseline[i] || now < started_at) {
          reset(now, counts);
          return rates;
        }
      }
      const auto elapsed = now - started_at;
      if (elapsed >= std::chrono::seconds(1)) {
        window_seconds = std::chrono::duration<double>(elapsed).count();
        rates_t updated {};
        for (std::size_t i = 0; i < counts.size(); ++i) {
          updated[i] = (counts[i] - baseline[i]) / window_seconds;
        }
        rates = updated;
        baseline = counts;
        started_at = now;
      }
      return rates;
    }

    double duration_seconds() const {
      return window_seconds;
    }

  private:
    clock_t::time_point started_at {};
    counts_t baseline {};
    std::optional<rates_t> rates;
    double window_seconds = 0.0;
  };
}  // namespace perf
