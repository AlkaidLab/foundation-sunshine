#pragma once

#include "transport_feedback.h"
#include "transport_policy.h"

#include <array>

namespace transport {
  enum class fec_selection_result_e { insufficient,
    unchanged,
    changed,
    best_effort,
    infeasible,
    invalid };

  struct fec_selection_config_t {
    std::array<std::uint32_t, 3> maximum_replay_failure_ppm { 1000, 100, 500 };
    unsigned minimum_parity = 0;
    std::size_t minimum_windows = 256;
    std::int64_t feedback_timeout_us = 1000000;
    std::int64_t decrease_stability_us = 10000000;
  };

  struct fec_selection_context_t {
    // Supplied from new covered data, never elapsed silence or report count.
    std::int64_t clean_covered_us = 0;
    std::int64_t since_last_change_us = 0;
  };

  struct fec_class_selection_t {
    fec_selection_result_e result = fec_selection_result_e::insufficient;
    unsigned percentage = 0;
    std::size_t replay_windows = 0;
    std::size_t replay_failures = 0;
    std::size_t block_shapes = 0;
    std::size_t frame_shapes = 0;
    std::uint32_t worst_frame_failure_ppm = 0;
    bool target_met = false;
  };

  struct fec_selection_t {
    std::array<fec_class_selection_t, 3> classes;
    bool valid = false;
    bool changed = false;
    bool infeasible = false;
    // Work diagnostics for this call only; no packet state survives the call.
    std::size_t geometries_evaluated = 0;
    std::size_t geometry_cache_hits = 0;
    std::size_t geometry_cache_bytes = 0;
  };

  // Pure bounded replay of authoritative raw network observations. Candidate
  // windows overlap: their failure fraction is empirical, not a statistical
  // confidence bound or proof of future delivery/deadline performance.
  // The caller owns arbitration, budget/application ordering and hysteresis.
  fec_selection_t
  select_fec(const protection_trace_t &trace, const frame_policy_t &policy,
    const fec_selection_config_t &config = {}, const fec_selection_context_t &context = {});
}  // namespace transport
