/**
 * @file src/platform/windows/d3d12/d3d12_hdr_statistics.h
 * @brief API-independent HDR analysis result layout and CPU summarization.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace platf::dxgi::d3d12 {
  inline constexpr std::size_t hdr_histogram_bins = 256;

  struct hdr_final_result_t {
    float min_maxrgb = 0.0f;
    float max_maxrgb = 0.0f;
    float sum_maxrgb = 0.0f;
    std::uint32_t pixel_count = 0;
    std::array<std::uint32_t, hdr_histogram_bins> histogram {};
  };
  static_assert(sizeof(hdr_final_result_t) == 1040);

  struct hdr_percentiles_t {
    float min_maxrgb = 0.0f;
    float max_maxrgb = 0.0f;
    float avg_maxrgb = 0.0f;
    float percentile_10_pq = 0.0f;
    float percentile_90_pq = 0.0f;
    float percentile_95_pq = 0.0f;
    float percentile_99_pq = 0.0f;
    bool valid = false;
  };

  [[nodiscard]] hdr_percentiles_t
  summarize_hdr_result(const hdr_final_result_t &result);
}  // namespace platf::dxgi::d3d12
