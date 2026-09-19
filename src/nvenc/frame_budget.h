/**
 * @file src/nvenc/frame_budget.h
 * @brief Frame budget guard: adaptive NVENC preset clamping for stream framerates.
 *
 * SDK-independent declarations (no nvEncodeAPI.h includes) so that non-NVENC
 * translation units (e.g. confighttp) can consume the evaluation report.
 */
#pragma once

#include <optional>

namespace nvenc {

  /**
   * @brief Result of evaluating the configured preset against the frame budget
   *        of a session. Produced by evaluate_frame_budget().
   */
  struct frame_budget_verdict {
    bool clamped = false;  ///< Effective preset had to be lowered to fit the budget.
    bool budget_exceeded_at_floor = false;  ///< Even P1 does not fit the allotted budget.
    int configured_preset = 0;
    int effective_preset = 0;
    int width = 0;
    int height = 0;
    double fps = 0.0;
    double budget_ms = 0.0;  ///< Allotted encode time per frame (fraction of the frame interval).
    double estimated_ms = 0.0;  ///< Estimated encode time at the effective preset.
    double configured_estimated_ms = 0.0;  ///< Estimated encode time at the configured preset.
    int num_engines = 1;
  };

  /**
   * @brief Evaluate whether `configured_preset` can keep up with the session framerate
   *        and lower it if it cannot. Pure function, no hardware access.
   * @param configured_preset NVENC performance preset 1-7.
   * @param width Encode width in pixels.
   * @param height Encode height in pixels.
   * @param fps Effective stream framerate.
   * @param num_engines Number of NVENC engines reported by the driver (>=1).
   * @param guard_enabled When false the configured preset is kept and the estimate
   *        fields describe it; the budget fields are still computed for reporting.
   */
  frame_budget_verdict
  evaluate_frame_budget(int configured_preset, int width, int height, double fps, int num_engines, bool guard_enabled);

  /**
   * @brief Publish the verdict of the latest real (non-probe) session so that the
   *        config API can surface the effective preset to the Web UI.
   */
  void
  publish_frame_budget_report(const frame_budget_verdict &verdict);

  /**
   * @brief Fetch the last published verdict, if any.
   */
  std::optional<frame_budget_verdict>
  get_frame_budget_report();

}  // namespace nvenc
