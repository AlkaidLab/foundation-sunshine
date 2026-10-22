/**
 * @file src/video_hdr_analyzer.h
 * @brief CPU-side per-pixel max(R,G,B) analysis of PQ frames for HDR dynamic metadata.
 *
 * The Windows analyzer runs a compute shader over scRGB frames and therefore sees
 * true per-pixel max(R,G,B). The Linux capture path delivers PQ YCbCr, so the same
 * quantity has to be reconstructed here: convert each sample triple to R'G'B' in the
 * PQ signal domain and take the largest of the three. Luma alone is an underestimate
 * for saturated primaries - pure red at PQ 0.5081 (100 nits) has a luma of about
 * 0.13, i.e. 1.2 nits - which is exactly what the HDR10+ maxSCL must not be based on.
 *
 * Only the PQ signal arithmetic lives here; the PQ transfer function itself comes from
 * video_hdr_metadata.h so the analyzer and the metadata writers cannot drift apart.
 */
#pragma once

#include "src/video_hdr_metadata.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace video::hdr_analyzer {
  /**
   * @brief One 16-bit sample plane. `stride` counts samples, not bytes.
   */
  struct plane_t {
    const std::uint16_t *data = nullptr;
    int stride = 0;
  };

  /**
   * @brief A PQ frame laid out the way the analyzer reads it.
   *
   * `shift` right-shifts every sample down to a 10-bit code: FFmpeg's planar P10
   * formats keep their codes in the low bits, while P010 and P16 keep them in the
   * high bits, and unpacking both with one shift reads a black frame.
   *
   * `chroma_subsample` is the factor on both axes (1 = 4:4:4, 2 = 4:2:0). Chroma is
   * read with nearest-neighbour siting; the analysis runs on a downscaled frame, so
   * interpolating here would cost more than the statistic is worth.
   */
  struct frame_t {
    plane_t y;
    plane_t cb;
    plane_t cr;
    int width = 0;
    int height = 0;
    int shift = 0;
    int chroma_subsample = 1;
    bool limited_range = true;
    /// BT.2020 non-constant-luminance coefficients; BT.709 otherwise.
    bool bt2020 = true;

    bool
    has_chroma() const {
      return cb.data && cr.data && cb.stride > 0 && cr.stride > 0;
    }
  };

  namespace detail {
    constexpr int code_max = 1023;
    constexpr int limited_y_low = 64;
    constexpr int limited_y_span = 940 - 64;
    constexpr int limited_c_center = 512;
    // Limited-range chroma spans [64, 960] around the 512 centre.
    constexpr int limited_c_span = 960 - 64;

    inline float
    luma_signal(std::uint16_t raw, int shift, bool limited) {
      const auto code = (raw >> shift) & code_max;
      if (limited) {
        return std::clamp((code - limited_y_low) / float(limited_y_span), 0.0f, 1.0f);
      }
      return code / float(code_max);
    }

    inline float
    chroma_signal(std::uint16_t raw, int shift, bool limited) {
      const auto code = (raw >> shift) & code_max;
      if (limited) {
        return (code - limited_c_center) / float(limited_c_span);
      }
      return (code - limited_c_center) / float(code_max);
    }

    /**
     * @brief max(R',G',B') from one Y'CbCr triple, in the PQ signal domain.
     * @details Inverse of the non-constant-luminance matrix. The coefficients are
     *          the exact reciprocals of the forward ones, so a grey triple returns
     *          its own luma (max(R,G,B) == Y when R == G == B).
     */
    inline float
    max_rgb_signal(float y, float cb, float cr, bool bt2020) {
      // R' = Y' + 2(1-Kr)Cr, B' = Y' + 2(1-Kb)Cb, G' = Y' - (Kr R' + Kb B')/Kg
      const float kr = bt2020 ? 0.2627f : 0.2126f;
      const float kb = bt2020 ? 0.0593f : 0.0722f;
      const float kg = 1.0f - kr - kb;

      const float r = y + 2.0f * (1.0f - kr) * cr;
      const float b = y + 2.0f * (1.0f - kb) * cb;
      const float g = y - (kr * r + kb * b - y * (kr + kb)) / kg;

      return std::max(r, std::max(g, b));
    }
  }  // namespace detail

  /**
   * @brief Fill `stats` from one frame and bump `sequence`.
   * @details Leaves `stats` untouched when the frame carries no usable samples, so an
   *          unreadable frame never publishes a zeroed statistic.
   */
  inline void
  analyze(const frame_t &frame, platf::hdr_frame_luminance_stats_t &stats, std::uint64_t &sequence) {
    using namespace detail;

    if (!frame.y.data || frame.y.stride <= 0 || frame.width <= 0 || frame.height <= 0) {
      return;
    }

    const auto limited = frame.limited_range;

    float signal_lut[code_max + 1];
    float nits_lut[code_max + 1];
    for (int code = 0; code <= code_max; ++code) {
      // The histogram is keyed by a 10-bit code; map it through the same range
      // remap the samples went through so both ends agree.
      auto c = code;
      if (limited) {
        c = ((code - limited_y_low) * code_max) / limited_y_span;
      }
      signal_lut[code] = std::clamp(c, 0, code_max) / float(code_max);
      nits_lut[code] = hdr_metadata::pq_to_nits(signal_lut[code]);
    }

    const bool per_pixel_maxrgb = frame.has_chroma();
    const int step = std::max(1, frame.chroma_subsample);

    // This runs on the encoding thread every few frames, so a 4K frame must not
    // cost 8.3M maxRGB reconstructions. Windows caps the same analysis at
    // hdr_analysis_max_width x hdr_analysis_max_height (its downsampled
    // texture); striding over the full-resolution frame is the CPU equivalent
    // and keeps the statistics comparable.
    const int row_step = std::max(1, (frame.height + hdr_metadata::hdr_analysis_max_height - 1) / hdr_metadata::hdr_analysis_max_height);
    const int col_step = std::max(1, (frame.width + hdr_metadata::hdr_analysis_max_width - 1) / hdr_metadata::hdr_analysis_max_width);

    std::vector<std::uint32_t> histogram(code_max + 1, 0);
    std::uint64_t sampled = 0;
    for (int row = 0; row < frame.height; row += row_step) {
      const auto *y_line = frame.y.data + (size_t) row * frame.y.stride;
      const auto *cb_line = per_pixel_maxrgb ? frame.cb.data + (size_t) (row / step) * frame.cb.stride : nullptr;
      const auto *cr_line = per_pixel_maxrgb ? frame.cr.data + (size_t) (row / step) * frame.cr.stride : nullptr;

      for (int col = 0; col < frame.width; col += col_step) {
        ++sampled;
        const auto y_code = (y_line[col] >> frame.shift) & code_max;
        if (!per_pixel_maxrgb) {
          ++histogram[y_code];
          continue;
        }

        const auto chroma_col = col / step;
        const auto signal = max_rgb_signal(
          luma_signal(y_line[col], frame.shift, limited),
          chroma_signal(cb_line[chroma_col], frame.shift, limited),
          chroma_signal(cr_line[chroma_col], frame.shift, limited),
          frame.bt2020);

        // Back to the histogram's key space: the same limited-range remap as the LUTs.
        auto code = (int) std::lround(std::clamp(signal, 0.0f, 1.0f) * float(code_max));
        if (limited) {
          code = limited_y_low + (code * limited_y_span) / code_max;
        }
        ++histogram[std::clamp(code, 0, code_max)];
      }
    }

    const auto total = sampled;
    if (!total) {
      return;
    }

    auto code_at_percentile = [&](int pct) {
      const auto target = (std::uint64_t) std::ceil(total * pct / 100.0);
      std::uint64_t acc = 0;
      for (int code = 0; code <= code_max; ++code) {
        acc += histogram[code];
        if (acc >= target) {
          return code;
        }
      }
      return code_max;
    };

    double nits_sum = 0.0;
    double signal_sum = 0.0;
    std::uint64_t near_black = 0;
    int min_code = -1;
    int max_code = 0;
    for (int code = 0; code <= code_max; ++code) {
      const auto count = histogram[code];
      if (!count) {
        continue;
      }
      if (min_code < 0) {
        min_code = code;
      }
      max_code = code;
      nits_sum += (double) count * nits_lut[code];
      signal_sum += (double) count * signal_lut[code];
      if (signal_lut[code] < 1.0f / 256.0f) {
        near_black += count;
      }
    }
    if (min_code < 0) {
      return;
    }

    stats.min_maxrgb = nits_lut[min_code];
    stats.max_maxrgb = nits_lut[max_code];
    stats.avg_maxrgb = (float) (nits_sum / total);
    stats.avg_maxrgb_pq = (float) (signal_sum / total);
    stats.near_black_fraction = (float) ((double) near_black / total);
    stats.near_black_stats_valid = true;
    // PQ's analysis clamp is the ST 2084 peak; when HLG analysis lands it must
    // follow the Windows rule instead (min(display peak, this peak)).
    stats.analysis_max_nits = hdr_metadata::st2084_peak_nits;
    stats.sample_sequence = ++sequence;
    stats.valid = true;

    for (size_t i = 0; i < hdr_metadata::hdr10plus_percentages.size(); ++i) {
      const auto pct = hdr_metadata::hdr10plus_percentages[i];
      const auto code = code_at_percentile(pct);
      stats.distribution_maxrgb[i] = nits_lut[code];
      switch (pct) {
        case 1:
          stats.percentile_1_pq = signal_lut[code];
          break;
        case 10:
          stats.percentile_10_pq = signal_lut[code];
          break;
        case 90:
          stats.percentile_90_pq = signal_lut[code];
          break;
        case 99:
          stats.percentile_99 = nits_lut[code];
          break;
        default:
          break;
      }
    }
  }
}  // namespace video::hdr_analyzer
