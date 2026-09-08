/**
 * @file src/platform/windows/hdr_analysis_result.cpp
 * @brief Decode GPU HDR statistics into common stream metadata.
 */
#include "hdr_analysis_result.h"
#include "src/video_hdr_metadata.h"

#include <algorithm>
#include <cmath>

namespace platf::dxgi::hdr_analysis {
  platf::hdr_frame_luminance_stats_t
  decode_result(const result_t &result, float max_analysis_nits, std::uint64_t sample_sequence) {
    platf::hdr_frame_luminance_stats_t stats {};
    if (result.pixel_count == 0) return stats;
    stats.min_maxrgb = result.min_maxrgb;
    stats.max_maxrgb = result.max_maxrgb;
    stats.avg_maxrgb = result.sum_maxrgb / static_cast<float>(result.pixel_count);
    // HDR Vivid's average is a PQ-domain statistic like the variance beside it, so
    // the GPU accumulates PQ(maxRGB) per pixel and this divides that sum. It cannot
    // be derived from avg_maxrgb above (PQ is concave, so PQ(mean) is far above
    // mean(PQ) on a dark frame with highlights) nor from the histogram below (that
    // is populated from one representative sample per analysis cell, which is a
    // distribution to take percentiles from, not an exact mean).
    //
    // std::clamp() alone would launder bad data: it passes a NaN straight through,
    // and it turns an implausible value into exactly 1.0, which reads downstream as
    // a legitimate "entire frame at 10,000 nits". So screen first and clamp only the
    // FP32 rounding overshoot a sum of in-range summands can produce. An implausible
    // value stays zero, which vivid_from_stats() reads as "the analyzer produced no
    // PQ average" and withholds Vivid on. The rest of the sample is still published:
    // HDR10+ does not read this field and carries its own statistics.
    const float mean_pq = result.sum_maxrgb_pq / static_cast<float>(result.pixel_count);
    stats.avg_maxrgb_pq =
      (std::isfinite(mean_pq) && mean_pq >= -0.001f && mean_pq <= 1.001f) ?
        std::clamp(mean_pq, 0.0f, 1.0f) :
        0.0f;

    // HDR Vivid defines variance as P90-P10 in normalized PQ signal space.
    // Retain P99 in nits for the independent HDR10+ path, and fill the nine
    // percentiles ST 2094-40 deployment profiles carry from the same walk.
    const uint32_t total = result.pixel_count;
    stats.near_black_fraction =
      static_cast<float>(result.histogram[0]) / static_cast<float>(total);
    stats.near_black_stats_valid = true;
    const auto &percentages = ::video::hdr_metadata::hdr10plus_percentages;
    constexpr size_t kDistCount = percentages.size();

    std::array<uint32_t, kDistCount> dist_targets {};
    std::array<bool, kDistCount> dist_found {};
    for (size_t p = 0; p < kDistCount; ++p) {
      dist_targets[p] = static_cast<uint32_t>(std::ceil(total * (percentages[p] / 100.0f)));
    }
    const uint32_t target_10 = static_cast<uint32_t>(std::ceil(total * 0.10f));
    const uint32_t target_90 = static_cast<uint32_t>(std::ceil(total * 0.90f));
    const uint32_t target_99 = static_cast<uint32_t>(std::ceil(total * 0.99f));
    uint32_t cumulative = 0;
    bool found_10 = false;
    bool found_90 = false;
    bool found_99 = false;

    for (uint32_t i = 0; i < histogram_bins; i++) {
      cumulative += result.histogram[i];
      const float pq_bin_center = (static_cast<float>(i) + 0.5f) / histogram_bins;
      for (size_t p = 0; p < kDistCount; ++p) {
        if (!dist_found[p] && cumulative >= dist_targets[p]) {
          stats.distribution_maxrgb[p] =
            ::video::hdr_metadata::pq_to_nits(pq_bin_center);
          if (p == 0) {
            stats.percentile_1_pq = pq_bin_center;
          }
          dist_found[p] = true;
        }
      }
      if (!found_10 && cumulative >= target_10) {
        stats.percentile_10_pq = pq_bin_center;
        found_10 = true;
      }
      if (!found_90 && cumulative >= target_90) {
        stats.percentile_90_pq = pq_bin_center;
        found_90 = true;
      }
      if (!found_99 && cumulative >= target_99) {
        stats.percentile_99 =
          ::video::hdr_metadata::pq_to_nits(pq_bin_center);
        found_99 = true;
      }
      // The 99th percentile is the last of every target set, so the walk can stop
      // once both it and the distribution are filled.
      if (found_99 && dist_found[kDistCount - 1]) {
        break;
      }
    }

    stats.analysis_max_nits = max_analysis_nits;
    stats.sample_sequence = sample_sequence;
    stats.valid = true;
    return stats;
  }
}  // namespace platf::dxgi::hdr_analysis
