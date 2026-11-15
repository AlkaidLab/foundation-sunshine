/**
 * @file src/telemetry/self_baseline.h
 * @brief A rolling baseline of this install's own stream-health statistics, and the one small event sent when they get clearly worse.
 *
 * Every non-idle window adds its statistics (p50 or p95 of the host latency, send duration, backlog, encode time and frame jitter) to a slow average kept in
 * <config>/telemetry/stream_baseline.json. After an update, or whenever the current windows shift, the install compares its last windows with its own baseline: a statistic that is at least three
 * times the baseline, and above it by a fixed amount, in five windows in a row, is reported once per version as `stream_regress` (metric, baseline, current). Nothing else leaves the computer.
 * Pure code (std + nlohmann).
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "stream_health.h"

namespace telemetry {

  struct baseline_event_t {
    std::string metric;  ///< a key of health::hist_keys()
    double baseline = 0;
    double current = 0;
    double delta = 0;
    int windows = 0;  ///< consecutive bad windows
    std::uint64_t baseline_windows = 0;
    bool version_changed = false;
  };

  class self_baseline_t {
  public:
    /** `file` may be empty (nothing is stored). `version` is the product version of this run. */
    self_baseline_t(std::filesystem::path file, std::string version);

    /** One non-idle window. -> the events to send (usually none). */
    std::vector<baseline_event_t>
    observe(const health::window_t &window);
    /** Write the baseline to its file (called when a stream ends and every few windows). */
    void
    save() const;

    static constexpr std::uint64_t MIN_BASELINE_WINDOWS = 30;
    static constexpr int CONSECUTIVE_WINDOWS = 5;
    static constexpr double RATIO = 3.0;
    static constexpr int MAX_EVENTS_PER_DAY = 3;

  private:
    struct metric_state_t {
      std::string key;
      double baseline = 0;
      std::uint64_t n = 0;
      std::string baseline_version;  ///< the version under which most of the baseline was collected
      std::string reported_version;  ///< the version this metric was last reported for
      int bad_run = 0;
      double run_sum = 0;
    };
    metric_state_t *
    find(std::string_view key);

    std::filesystem::path file_;
    std::string version_;
    std::vector<metric_state_t> metrics_;
    int windows_since_save_ = 0;
    std::int64_t day_ = -1;
    int events_today_ = 0;
  };

}  // namespace telemetry
