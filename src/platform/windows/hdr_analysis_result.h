/**
 * @file src/platform/windows/hdr_analysis_result.h
 * @brief Shared D3D11/D3D12 HDR result ABI and metadata decoding.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace platf {
  struct hdr_frame_luminance_stats_t;
}

namespace platf::dxgi::hdr_analysis {
  inline constexpr std::size_t histogram_bins = 256;

  struct result_t {
    float min_maxrgb = 0.0f;
    float max_maxrgb = 0.0f;
    float sum_maxrgb = 0.0f;
    float sum_maxrgb_pq = 0.0f;
    std::uint32_t pixel_count = 0;
    std::array<std::uint32_t, histogram_bins> histogram {};
  };
  static_assert(sizeof(result_t) == 1044);
  static_assert(offsetof(result_t, pixel_count) == 16);
  static_assert(offsetof(result_t, histogram) == 20);

  [[nodiscard]] platf::hdr_frame_luminance_stats_t
  decode_result(const result_t &result, float max_analysis_nits, std::uint64_t sample_sequence);
}  // namespace platf::dxgi::hdr_analysis
