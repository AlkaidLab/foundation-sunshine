/**
 * @file src/nvenc/frame_budget.cpp
 * @brief Definitions for the frame budget guard estimator and report store.
 */
#include "frame_budget.h"

#include <mutex>

namespace nvenc {

  namespace {
    /**
     * Estimated encode time per preset in milliseconds per megapixel, indexed by
     * preset number (single NVENC engine). Calibrated from community RTX 50-series
     * measurements (4K60: P1~5.3ms, P7~10.4ms per frame), rounded up to stay
     * conservative on older GPU generations. The guard is a heuristic; users can
     * disable it via `nvenc_frame_budget_guard`.
     */
    constexpr double encode_ms_per_mpix[8] = { 0.0, 0.7, 0.8, 0.9, 1.0, 1.1, 1.2, 1.4 };

    /**
     * Fraction of the frame interval we allow encoding to consume. The rest of the
     * budget belongs to capture, game render and network transmission.
     */
    constexpr double frame_budget_utilization = 0.65;

    /**
     * Throughput multiplier when the GPU exposes more than one NVENC engine
     * (split-frame encoding can spread a frame across engines).
     */
    constexpr double multi_engine_speedup = 1.75;
  }  // namespace

  frame_budget_verdict
  evaluate_frame_budget(int configured_preset, int width, int height, double fps, int num_engines, bool guard_enabled) {
    frame_budget_verdict verdict;
    verdict.configured_preset = configured_preset;
    verdict.effective_preset = configured_preset;
    verdict.width = width;
    verdict.height = height;
    verdict.fps = fps;
    verdict.num_engines = num_engines;

    if (configured_preset < 1) configured_preset = 1;
    if (configured_preset > 7) configured_preset = 7;
    if (fps <= 0.0 || width <= 0 || height <= 0) {
      // Not enough information to reason about; keep the configured preset.
      return verdict;
    }

    verdict.configured_preset = configured_preset;
    verdict.effective_preset = configured_preset;
    verdict.budget_ms = 1000.0 / fps * frame_budget_utilization;

    const double mpix = static_cast<double>(width) * height / 1e6;
    const double speedup = num_engines > 1 ? multi_engine_speedup : 1.0;
    verdict.configured_estimated_ms = encode_ms_per_mpix[configured_preset] * mpix / speedup;
    verdict.estimated_ms = verdict.configured_estimated_ms;

    int chosen = 1;
    for (int preset = configured_preset; preset >= 1; --preset) {
      const double estimated_ms = encode_ms_per_mpix[preset] * mpix / speedup;
      if (estimated_ms <= verdict.budget_ms || preset == 1) {
        chosen = preset;
        verdict.estimated_ms = estimated_ms;
        break;
      }
    }

    if (chosen < configured_preset) {
      verdict.clamped = true;
      verdict.effective_preset = chosen;
    }
    if (verdict.estimated_ms > verdict.budget_ms) {
      verdict.budget_exceeded_at_floor = true;
    }
    if (!guard_enabled) {
      // Guard disabled: the configured preset stands; still report the budget
      // numbers so the config API describes the session truthfully.
      verdict.clamped = false;
      verdict.effective_preset = verdict.configured_preset;
      verdict.estimated_ms = verdict.configured_estimated_ms;
    }
    return verdict;
  }

  namespace {
    std::mutex report_mutex;
    std::optional<frame_budget_verdict> stored_report;
  }  // namespace

  void
  publish_frame_budget_report(const frame_budget_verdict &verdict) {
    std::lock_guard<std::mutex> lock(report_mutex);
    stored_report = verdict;
  }

  std::optional<frame_budget_verdict>
  get_frame_budget_report() {
    std::lock_guard<std::mutex> lock(report_mutex);
    return stored_report;
  }

}  // namespace nvenc
