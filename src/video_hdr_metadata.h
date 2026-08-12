/**
 * @file src/video_hdr_metadata.h
 * @brief Helpers for generating and stabilizing HDR dynamic metadata.
 */
#pragma once

#include "platform/common.h"
#include "video_colorspace.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace video::hdr_metadata {

  namespace detail {
    constexpr double st2084_m1 = 2610.0 / 4096.0 / 4.0;
    constexpr double st2084_m2 = 2523.0 / 4096.0 * 128.0;
    constexpr double st2084_c1 = 3424.0 / 4096.0;
    constexpr double st2084_c2 = 2413.0 / 4096.0 * 32.0;
    constexpr double st2084_c3 = 2392.0 / 4096.0 * 32.0;
  }  // namespace detail

  struct formats_t {
    bool hdr10plus = false;
    bool vivid = false;
  };

  constexpr int hdr10plus_normalized_scale = 100000;
  constexpr uint8_t hdr10plus_application_version = 1;
  constexpr size_t hdr10plus_t35_prefix_size = 6;

  /**
   * The SMPTE ST 2084 reference peak that every ST 2094-40 luminance field is
   * normalized against.
   *
   * maxSCL, average_maxrgb and the maxRGB distribution describe absolute content
   * luminance as a fraction of this peak, never a fraction of the target display.
   * A consumer recovers nits by multiplying straight back out — libplacebo does
   * `scene_max[i] = 10000 * av_q2d(maxscl[i])` in pl_map_hdr_metadata — so
   * normalizing against anything else scales the whole picture by the ratio.
   * The target display belongs in targeted_system_display_maximum_luminance,
   * which is a separate, non-normalized field carried in nits.
   */
  constexpr float hdr10plus_pq_reference_nits = 10000.0f;
  // Large enough for FFmpeg's maximum ST 2094-40 body plus the T.35 prefix,
  // without exposing FFmpeg headers to this shared, unit-testable header.
  constexpr size_t hdr10plus_t35_max_payload_size = 1024;

  /**
   * The maxRGB percentages ST 2094-40 deployment profiles carry.
   *
   * The syntax element num_distribution_maxrgb_percentiles is u(4), so a zero count
   * is representable, but shipping HDR10+ always sends these nine. FFmpeg neither
   * enforces nor round-trips the count, so an empty distribution serializes and
   * parses back cleanly while still being non-conformant on the wire.
   */
  inline constexpr std::array<uint8_t, 9> hdr10plus_percentages { 1, 5, 10, 25, 50, 75, 90, 95, 99 };

  /**
   * ST 2094-40 8.5.4 excludes the 5% and 10% slots from the CDF in
   * application_version 1 and reserves them at the fixed values V1 = 0.00000 and
   * V2 = 0.00255. ST 2094-50 redefines the same two slots as V1 = scene luminance
   * at 99.99% of the frame and V2 = percentage of pixels at or below 100 nits, so
   * a consumer that finds a nonzero V1 reads it as the frame peak.
   *
   * libplacebo does exactly that: pl_map_hdr_metadata derives max_pq_y from slot 1
   * whenever application_version is 1 with the nine standard percentages, then
   * pl_color_space_nominal_luma_ex prefers those CIE-Y values over maxSCL. Writing
   * a real 5th percentile there reports the darkest part of the frame as its peak,
   * which is why the V1 = 0 sentinel exists — it is how a conformant stream tells
   * the consumer to fall back to maxSCL.
   *
   * The analyzer's 5% and 10% percentiles are therefore computed but not carried.
   * The slots stay in the table so the array keeps lining up with
   * hdr10plus_percentages and with the analyzer's distribution_maxrgb[].
   */
  constexpr size_t hdr10plus_reserved_v1_index = 1;
  constexpr size_t hdr10plus_reserved_v2_index = 2;
  constexpr int hdr10plus_reserved_v1 = 0;  // 0.00000
  constexpr int hdr10plus_reserved_v2 = 255;  // 0.00255 x hdr10plus_normalized_scale

  static_assert(hdr10plus_percentages[hdr10plus_reserved_v1_index] == 5 &&
                  hdr10plus_percentages[hdr10plus_reserved_v2_index] == 10,
    "the reserved ST 2094-40 slots are the 5% and 10% percentages");

  static_assert(
    hdr10plus_percentages.size() == platf::hdr_frame_luminance_stats_t::HDR10PLUS_PERCENTILES,
    "analyzer distribution_maxrgb[] must match the HDR10+ percentage table");

  struct hdr10plus_frame_metadata_t {
    int maxscl = 0;
    int average_maxrgb = 0;
    /// Normalized maxRGB at each entry of hdr10plus_percentages.
    std::array<int, hdr10plus_percentages.size()> distribution_maxrgb {};
    uint16_t targeted_system_display_maximum_luminance = 1000;
    bool valid = false;
  };

  /**
   * Convert analyzer luminance values into the normalized ST 2094-40 fields
   * shared by the AVCodec side-data and native NVENC paths.
   *
   * distribution carries maxRGB in nits at each hdr10plus_percentages entry. A null
   * pointer, or any non-finite or negative entry, leaves the distribution at zero —
   * callers must then omit it rather than send a partially filled one.
   */
  inline hdr10plus_frame_metadata_t
  hdr10plus_from_luminance(float percentile_95, float average_maxrgb,
    uint16_t max_display_luminance, const float *distribution = nullptr) {
    if (!std::isfinite(percentile_95) || !std::isfinite(average_maxrgb) ||
        percentile_95 < 0.0f || average_maxrgb < 0.0f) {
      return {};
    }

    const uint16_t target_nits = std::clamp<uint16_t>(
      max_display_luminance > 0 ? max_display_luminance : 1000,
      1,
      10000);
    // Absolute, display-independent: see hdr10plus_pq_reference_nits. target_nits
    // deliberately plays no part here — it is only reported as the targeted system
    // display maximum luminance below.
    const auto normalize = [](float nits) {
      const float normalized = std::clamp(nits / hdr10plus_pq_reference_nits, 0.0f, 1.0f);
      return static_cast<int>(std::lround(normalized * hdr10plus_normalized_scale));
    };

    hdr10plus_frame_metadata_t result {
      .maxscl = normalize(percentile_95),
      .average_maxrgb = normalize(average_maxrgb),
      .targeted_system_display_maximum_luminance = target_nits,
      .valid = true,
    };

    if (distribution) {
      for (size_t i = 0; i < hdr10plus_percentages.size(); ++i) {
        if (!std::isfinite(distribution[i]) || distribution[i] < 0.0f) {
          result.distribution_maxrgb = {};
          return result;
        }
        result.distribution_maxrgb[i] = normalize(distribution[i]);
      }

      // Overwrite the two reserved slots last, so an analyzer percentile can never
      // reach the wire there. An all-zero distribution from the rejection path
      // above is left alone: V1 = 0 is the sentinel that makes a consumer fall
      // back to maxSCL, which is what we want when the analysis is unusable.
      if constexpr (hdr10plus_application_version == 1) {
        result.distribution_maxrgb[hdr10plus_reserved_v1_index] = hdr10plus_reserved_v1;
        result.distribution_maxrgb[hdr10plus_reserved_v2_index] = hdr10plus_reserved_v2;
      }
    }

    return result;
  }

  /**
   * Serialize one frame of HDR10+ metadata as a complete registered ITU-T T.35
   * payload, ready for an HEVC SEI message or an AV1 metadata OBU.
   * Invalid input, insufficient output space, or a failed FFmpeg round trip
   * returns zero.
   */
  size_t
  serialize_hdr10plus_t35(const platf::hdr_frame_luminance_stats_t &stats,
    uint16_t max_display_luminance,
    std::span<uint8_t> payload);

  /**
   * Which dynamic metadata formats may be emitted for this stream.
   *
   * Two independent gates. The transfer function decides what may describe the
   * content: HDR10+ carries absolute luminance so it is PQ-only, while HDR Vivid
   * covers both PQ and HLG (T/UWA 005.1-2024 clause 7).
   *
   * The codec decides what may be written. HDR Vivid defines a carriage only for
   * AVS2 (clause 8) and HEVC/VVC (annex B) — the standard never mentions AV1 or
   * OBUs, so emitting it there invents a mapping no decoder is obliged to accept.
   * HDR10+ does have one, from AOMedia's HDR10+ AV1 Metadata Handling
   * Specification, so it is not codec-gated here.
   *
   * video_format follows the config_t::videoFormat convention: 0 H.264, 1 HEVC, 2 AV1.
   */
  inline formats_t
  formats_for(const sunshine_colorspace_t &colorspace, int video_format) {
    const bool vivid_carriable = (video_format == 1);
    switch (colorspace.colorspace) {
      case colorspace_e::bt2020:
        return { .hdr10plus = true, .vivid = vivid_carriable };
      case colorspace_e::bt2020hlg:
        return { .hdr10plus = false, .vivid = vivid_carriable };
      default:
        return {};
    }
  }

  /**
   * Whether stream startup should hold frames back until the HDR Vivid startup
   * guard reports stable analyzer output.
   *
   * Only HLG needs it: a plain-HLG IDR followed by a mid-stream switch into Vivid
   * is visible to the client. The wait is pointless when Vivid is never emitted
   * for this codec, and would only delay the first frame.
   */
  inline bool
  needs_vivid_startup_preroll(
    const sunshine_colorspace_t &colorspace,
    int video_format,
    bool analysis_available) {
    return analysis_available &&
           colorspace.colorspace == colorspace_e::bt2020hlg &&
           formats_for(colorspace, video_format).vivid;
  }
  /**
   * Convert absolute display luminance to the normalized SMPTE ST 2084 signal
   * used by GB/T 46269.1-2025 (equivalent to T/UWA 005.1-2024).
   */
  inline float
  nits_to_pq(float nits) {
    if (!std::isfinite(nits)) {
      return 0.0f;
    }

    const double normalized = std::clamp(static_cast<double>(nits) / 10000.0, 0.0, 1.0);
    const double powered = std::pow(normalized, detail::st2084_m1);
    return static_cast<float>(std::pow(
      (detail::st2084_c1 + detail::st2084_c2 * powered) /
        (1.0 + detail::st2084_c3 * powered),
      detail::st2084_m2
    ));
  }

  inline float
  pq_to_nits(float pq) {
    if (!std::isfinite(pq)) {
      return 0.0f;
    }

    const double powered =
      std::pow(std::clamp(static_cast<double>(pq), 0.0, 1.0), 1.0 / detail::st2084_m2);
    const double numerator = std::max(powered - detail::st2084_c1, 0.0);
    const double denominator =
      std::max(detail::st2084_c2 - detail::st2084_c3 * powered, 1.0e-12);
    return static_cast<float>(
      10000.0 * std::pow(numerator / denominator, 1.0 / detail::st2084_m1)
    );
  }

  inline uint16_t
  pq_to_u12(float pq) {
    if (!std::isfinite(pq)) {
      return 0;
    }
    return static_cast<uint16_t>(std::clamp(pq, 0.0f, 1.0f) * 4095.0f);
  }

  /**
   * Denominator paired with every 12-bit code value in this namespace.
   *
   * FFmpeg parses the CUVA fields as `(AVRational){get_bits(gb, 12), 4095}` — see
   * maxrgb_den and maximum_luminance_den in libavcodec/dynamic_hdr_vivid.c.
   */
  constexpr int pq_u12_den = 4095;

  /**
   * @brief Target display peak luminance as a 12-bit PQ code value.
   *
   * Pair with pq_u12_den when building an AVRational. Returning the raw code rather
   * than an AVRational keeps this header free of FFmpeg includes, which is what lets
   * it be unit-tested without the full media stack.
   *
   * @param nits Display peak luminance; values at or below zero fall back to 1000 nits,
   *             matching what the rest of the pipeline assumes for an unreported peak.
   */
  inline uint16_t
  target_display_pq_u12(float nits) {
    return pq_to_u12(nits_to_pq(nits > 0.0f ? nits : 1000.0f));
  }

  struct vivid_metadata_t {
    uint16_t minimum_maxrgb_pq = 0;
    uint16_t average_maxrgb_pq = 0;
    uint16_t variance_maxrgb_pq = 0;
    uint16_t maximum_maxrgb_pq = 0;
    bool valid = false;
  };

  /**
   * Generate the four statistics-mode HDR Vivid fields from content values.
   *
   * The fields describe the content in the PQ signal domain. Target display
   * luminance is intentionally not an input: it belongs to display adaptation
   * and optional curve parameters, not to these content statistics.
   */
  inline vivid_metadata_t
  vivid_from_stats(const platf::hdr_frame_luminance_stats_t &stats) {
    if (!stats.valid) {
      return {};
    }

    vivid_metadata_t result;
    result.minimum_maxrgb_pq = pq_to_u12(nits_to_pq(stats.min_maxrgb));
    result.average_maxrgb_pq = pq_to_u12(nits_to_pq(stats.avg_maxrgb));
    result.variance_maxrgb_pq = pq_to_u12(
      std::max(stats.percentile_90_pq - stats.percentile_10_pq, 0.0f));
    result.maximum_maxrgb_pq = pq_to_u12(nits_to_pq(stats.max_maxrgb));
    result.valid = true;
    return result;
  }

  /**
   * GB/T 46269.1-2025 Annex A.9 recommends a 32-frame arithmetic mean over
   * generated dynamic metadata. Reused analyzer samples are deliberately added
   * once per encoded frame so the window remains 32 video frames even when GPU
   * analysis runs at a lower cadence. reset() is available for a future reliable
   * scene-cut signal; this layer deliberately does not guess one from brightness.
   */
  class vivid_temporal_filter_t {
  public:
    vivid_metadata_t
    update(const platf::hdr_frame_luminance_stats_t &stats) {
      return update(vivid_from_stats(stats));
    }

    vivid_metadata_t
    update(const vivid_metadata_t &metadata) {
      if (!metadata.valid) {
        return {};
      }

      if (count_ == WINDOW_SIZE) {
        subtract(samples_[next_]);
      }
      else {
        ++count_;
      }

      samples_[next_] = metadata;
      add(metadata);
      next_ = (next_ + 1) % WINDOW_SIZE;

      vivid_metadata_t result;
      result.minimum_maxrgb_pq = static_cast<uint16_t>(minimum_sum_ / count_);
      result.average_maxrgb_pq = static_cast<uint16_t>(average_sum_ / count_);
      result.variance_maxrgb_pq = static_cast<uint16_t>(variance_sum_ / count_);
      result.maximum_maxrgb_pq = static_cast<uint16_t>(maximum_sum_ / count_);
      result.valid = true;
      return result;
    }

    void
    reset() {
      *this = {};
    }

  private:
    static constexpr size_t WINDOW_SIZE = 32;

    void
    add(const vivid_metadata_t &metadata) {
      minimum_sum_ += metadata.minimum_maxrgb_pq;
      average_sum_ += metadata.average_maxrgb_pq;
      variance_sum_ += metadata.variance_maxrgb_pq;
      maximum_sum_ += metadata.maximum_maxrgb_pq;
    }

    void
    subtract(const vivid_metadata_t &metadata) {
      minimum_sum_ -= metadata.minimum_maxrgb_pq;
      average_sum_ -= metadata.average_maxrgb_pq;
      variance_sum_ -= metadata.variance_maxrgb_pq;
      maximum_sum_ -= metadata.maximum_maxrgb_pq;
    }

    std::array<vivid_metadata_t, WINDOW_SIZE> samples_ {};
    size_t next_ = 0;
    uint32_t count_ = 0;
    uint32_t minimum_sum_ = 0;
    uint32_t average_sum_ = 0;
    uint32_t variance_sum_ = 0;
    uint32_t maximum_sum_ = 0;
  };

  /**
   * Gates HDR Vivid at stream startup until several independent GPU readbacks
   * describe a sane, stable HLG picture. The caller owns the wall-clock timeout
   * because timeout policy is a streaming concern, not metadata validation.
   */
  class vivid_startup_guard_t {
  public:
    bool
    observe(const platf::hdr_frame_luminance_stats_t &stats) {
      const auto accepted_end = accepted_sequences_.begin() + consecutive_samples_;
      if (ready_ || !stats.valid ||
          std::find(accepted_sequences_.begin(), accepted_end, stats.sample_sequence) != accepted_end) {
        return ready_;
      }

      if (!is_sane(stats)) {
        consecutive_samples_ = 0;
        previous_ = {};
        return false;
      }

      if (previous_.valid && !is_stable(previous_, stats)) {
        consecutive_samples_ = 1;
        accepted_sequences_[0] = stats.sample_sequence;
      }
      else {
        accepted_sequences_[consecutive_samples_] = stats.sample_sequence;
        ++consecutive_samples_;
      }
      previous_ = stats;

      if (consecutive_samples_ >= REQUIRED_SAMPLES) {
        ready_ = true;
      }
      return ready_;
    }

    uint32_t
    consecutive_samples() const {
      return consecutive_samples_;
    }

    static constexpr uint32_t REQUIRED_SAMPLES = 3;

  private:
    static bool
    is_sane(const platf::hdr_frame_luminance_stats_t &stats) {
      const bool finite =
        std::isfinite(stats.min_maxrgb) &&
        std::isfinite(stats.avg_maxrgb) &&
        std::isfinite(stats.max_maxrgb) &&
        std::isfinite(stats.percentile_10_pq) &&
        std::isfinite(stats.percentile_90_pq) &&
        std::isfinite(stats.analysis_max_nits);
      if (!finite || stats.analysis_max_nits <= 0.0f) {
        return false;
      }

      const float luminance_slack = std::max(1.0f, stats.analysis_max_nits * 0.01f);
      return stats.min_maxrgb >= 0.0f &&
             stats.max_maxrgb > 1.0f &&
             stats.avg_maxrgb + luminance_slack >= stats.min_maxrgb &&
             stats.max_maxrgb + luminance_slack >= stats.avg_maxrgb &&
             stats.max_maxrgb <= stats.analysis_max_nits + luminance_slack &&
             stats.percentile_10_pq >= 0.0f &&
             stats.percentile_90_pq <= 1.0f &&
             stats.percentile_10_pq <= stats.percentile_90_pq;
    }

    static bool
    scalar_is_stable(float previous, float current, float absolute_floor) {
      const float scale = std::max({ std::abs(previous), std::abs(current), absolute_floor });
      return std::abs(previous - current) <= scale * 0.5f;
    }

    static bool
    is_stable(
      const platf::hdr_frame_luminance_stats_t &previous,
      const platf::hdr_frame_luminance_stats_t &current) {
      return scalar_is_stable(previous.avg_maxrgb, current.avg_maxrgb, 20.0f) &&
             scalar_is_stable(previous.max_maxrgb, current.max_maxrgb, 50.0f) &&
             std::abs(previous.percentile_10_pq - current.percentile_10_pq) <= 0.15f &&
             std::abs(previous.percentile_90_pq - current.percentile_90_pq) <= 0.15f;
    }

    platf::hdr_frame_luminance_stats_t previous_ {};
    std::array<uint64_t, REQUIRED_SAMPLES> accepted_sequences_ {};
    uint32_t consecutive_samples_ = 0;
    bool ready_ = false;
  };

  namespace detail {

    class bit_writer_t {
    public:
      explicit bit_writer_t(std::vector<uint8_t> &buffer):
          buffer_(buffer) {
      }

      void
      write(uint32_t value, int bit_count) {
        for (int bit = bit_count - 1; bit >= 0; --bit) {
          accumulator_ = (accumulator_ << 1) | ((value >> bit) & 1U);
          ++pending_bits_;
          if (pending_bits_ == 8) {
            buffer_.push_back(static_cast<uint8_t>(accumulator_));
            accumulator_ = 0;
            pending_bits_ = 0;
          }
        }
      }

      void
      flush() {
        if (pending_bits_ == 0) {
          return;
        }

        accumulator_ <<= 8 - pending_bits_;
        buffer_.push_back(static_cast<uint8_t>(accumulator_));
        accumulator_ = 0;
        pending_bits_ = 0;
      }

    private:
      std::vector<uint8_t> &buffer_;
      uint32_t accumulator_ = 0;
      int pending_bits_ = 0;
    };

  }  // namespace detail

  /**
   * Serialize a CUVA HDR Vivid ITU-T T.35 payload.
   *
   * For system_start_code 0x01, T/UWA 005.1 fixes num_windows to one; it is not
   * present in the bitstream. tone_mapping_param_num is likewise absent when
   * tone_mapping_mode_flag is zero.
   */
  inline size_t
  serialize_vivid_t35(const vivid_metadata_t &metadata, std::vector<uint8_t> &payload) {
    if (!metadata.valid) {
      payload.clear();
      return 0;
    }

    payload.clear();
    payload.reserve(16);
    payload.push_back(0x26);  // itu_t_t35_country_code: China
    payload.push_back(0x00);
    payload.push_back(0x04);  // itu_t_t35_terminal_provider_code: CUVA
    payload.push_back(0x00);
    payload.push_back(0x05);  // itu_t_t35_terminal_provider_oriented_code
    payload.push_back(0x01);  // system_start_code

    detail::bit_writer_t writer { payload };
    writer.write(metadata.minimum_maxrgb_pq, 12);
    writer.write(metadata.average_maxrgb_pq, 12);
    writer.write(metadata.variance_maxrgb_pq, 12);
    writer.write(metadata.maximum_maxrgb_pq, 12);
    writer.write(0, 1);  // tone_mapping_mode_flag
    writer.write(0, 1);  // color_saturation_mapping_flag
    writer.flush();

    return payload.size();
  }

}  // namespace video::hdr_metadata
