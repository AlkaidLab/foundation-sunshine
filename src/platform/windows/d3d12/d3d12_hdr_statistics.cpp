/**
 * @file src/platform/windows/d3d12/d3d12_hdr_statistics.cpp
 * @brief API-independent HDR analysis result summarization.
 */
#include "d3d12_hdr_statistics.h"

#include <cmath>

namespace platf::dxgi::d3d12 {
  hdr_percentiles_t
  summarize_hdr_result(const hdr_final_result_t &result) {
    hdr_percentiles_t summary;
    if (result.pixel_count == 0) {
      return summary;
    }

    summary.min_maxrgb = result.min_maxrgb;
    summary.max_maxrgb = result.max_maxrgb;
    summary.avg_maxrgb =
      result.sum_maxrgb / static_cast<float>(result.pixel_count);

    const auto target = [&](float percentile) {
      return static_cast<std::uint32_t>(
        std::ceil(static_cast<float>(result.pixel_count) * percentile));
    };
    const auto target_10 = target(0.10f);
    const auto target_90 = target(0.90f);
    const auto target_95 = target(0.95f);
    const auto target_99 = target(0.99f);
    std::uint64_t cumulative = 0;
    for (std::size_t index = 0; index < hdr_histogram_bins; ++index) {
      cumulative += result.histogram[index];
      const auto center =
        (static_cast<float>(index) + 0.5f) /
        static_cast<float>(hdr_histogram_bins);
      if (summary.percentile_10_pq == 0.0f && cumulative >= target_10) {
        summary.percentile_10_pq = center;
      }
      if (summary.percentile_90_pq == 0.0f && cumulative >= target_90) {
        summary.percentile_90_pq = center;
      }
      if (summary.percentile_95_pq == 0.0f && cumulative >= target_95) {
        summary.percentile_95_pq = center;
      }
      if (summary.percentile_99_pq == 0.0f && cumulative >= target_99) {
        summary.percentile_99_pq = center;
        break;
      }
    }
    summary.valid = true;
    return summary;
  }
}  // namespace platf::dxgi::d3d12
