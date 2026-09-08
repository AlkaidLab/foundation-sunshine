/**
 * @file src/platform/windows/d3d12/d3d12_hdr_statistics.h
 * @brief API-independent HDR analysis result layout and CPU summarization.
 */
#pragma once

#include "src/platform/windows/hdr_analysis_result.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace platf::dxgi::d3d12 {
  inline constexpr auto hdr_histogram_bins = hdr_analysis::histogram_bins;
  using hdr_final_result_t = hdr_analysis::result_t;

  struct hdr_percentiles_t {
    float min_maxrgb = 0.0f;
    float max_maxrgb = 0.0f;
    float avg_maxrgb = 0.0f;
    float avg_maxrgb_pq = 0.0f;
    float percentile_10_pq = 0.0f;
    float percentile_90_pq = 0.0f;
    float percentile_95_pq = 0.0f;
    float percentile_99_pq = 0.0f;
    bool valid = false;
  };

  [[nodiscard]] hdr_percentiles_t
  summarize_hdr_result(const hdr_final_result_t &result);
}  // namespace platf::dxgi::d3d12
