/**
 * @file src/video_hdr_metadata.h
 * @brief Helpers for generating and stabilizing HDR dynamic metadata.
 */
#pragma once

#include "platform/common.h"
#include "video_colorspace.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
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

    /**
     * Whether the stream can carry any dynamic metadata at all.
     *
     * A false here is what tells the capture path to skip per-frame luminance
     * analysis: nothing downstream could use the result. HLG over AV1 is the
     * combination that lands here — HDR10+ is PQ-only and HDR Vivid has no AV1
     * carriage — even though the stream is genuinely HDR.
     */
    constexpr bool
    any() const {
      return hdr10plus || vivid;
    }

    /**
     * The formats left when two independent verdicts are combined: what the stream
     * allows, and what the encoder can actually write. Both have to say yes.
     */
    constexpr formats_t
    intersect(const formats_t &other) const {
      return {
        .hdr10plus = hdr10plus && other.hdr10plus,
        .vivid = vivid && other.vivid,
      };
    }
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
   * peak_maxrgb is reported as maxSCL. ST 2094-40 defines that field as the window's
   * maximum component, but the analyzer's true maximum is not the right thing to send
   * here: the source is a Windows compositor scRGB surface whose specular overshoot is
   * unbounded, and the PQ analysis path only clamps it at the 10000-nit ST 2084 peak,
   * so a handful of pixels can pin maxSCL at 1.0 and make every consumer tone-map the
   * whole scene against a 10000-nit peak. Callers pass the 99th percentile, which is
   * also the largest value this message's own CDF carries — maxSCL must not sit below
   * a percentile transmitted beside it, which is what the 95th percentile produced.
   *
   * distribution carries maxRGB in nits at each hdr10plus_percentages entry. A null
   * pointer leaves the distribution at zero. Any non-finite or negative entry
   * discards the whole distribution the same way. Either way the reserved slots keep
   * their fixed 8.5.4 values: V1 = 0 tells a consumer to fall back to maxSCL, which
   * stays valid, so the frame is still worth sending.
   */
  inline hdr10plus_frame_metadata_t
  hdr10plus_from_luminance(float peak_maxrgb, float average_maxrgb,
    uint16_t max_display_luminance, const float *distribution = nullptr) {
    if (!std::isfinite(peak_maxrgb) || !std::isfinite(average_maxrgb) ||
        peak_maxrgb < 0.0f || average_maxrgb < 0.0f) {
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
      .maxscl = normalize(peak_maxrgb),
      .average_maxrgb = normalize(average_maxrgb),
      .targeted_system_display_maximum_luminance = target_nits,
      .valid = true,
    };

    if (distribution) {
      for (size_t i = 0; i < hdr10plus_percentages.size(); ++i) {
        if (!std::isfinite(distribution[i]) || distribution[i] < 0.0f) {
          result.distribution_maxrgb = {};
          result.maxscl = normalize(peak_maxrgb);
          break;
        }
        result.distribution_maxrgb[i] = normalize(distribution[i]);
        // Structural, not incidental: whatever a caller reports as the peak, maxSCL
        // ends up at or above every percentile this message carries. The reserved
        // slots below are fixed sentinels rather than measurements, so they are the
        // one thing that must not raise it.
        if (i != hdr10plus_reserved_v1_index && i != hdr10plus_reserved_v2_index) {
          result.maxscl = std::max(result.maxscl, result.distribution_maxrgb[i]);
        }
      }
    }

    // Overwrite the two reserved slots last, so an analyzer percentile can never
    // reach the wire there. 8.5.4 fixes V1 and V2 unconditionally in
    // application_version 1, so this has to run on every path out of here, not just
    // the one that filled a distribution in: the serializer always emits all nine
    // percentiles, so a caller that passes no distribution at all would otherwise
    // ship V2 = 0. V1 stays 0 either way, which is the sentinel that makes a
    // consumer fall back to maxSCL — what we want when there is no usable analysis.
    if constexpr (hdr10plus_application_version == 1) {
      result.distribution_maxrgb[hdr10plus_reserved_v1_index] = hdr10plus_reserved_v1;
      result.distribution_maxrgb[hdr10plus_reserved_v2_index] = hdr10plus_reserved_v2;
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

  namespace detail {
    inline bool
    has_valid_scene_metrics(const platf::hdr_frame_luminance_stats_t &stats) {
      return stats.valid &&
             std::isfinite(stats.avg_maxrgb_pq) &&
             std::isfinite(stats.percentile_10_pq) &&
             std::isfinite(stats.percentile_90_pq);
    }
  }  // namespace detail

  /**
   * Detect metadata-relevant scene discontinuities without encoder lookahead.
   *
   * The analyzer runs below the video frame rate, so only a new sample_sequence
   * may advance this state. Comparing repeated samples would turn one transition
   * into several scene refreshes. The decision is made in PQ space from the mean,
   * low/high percentiles, and the HDR10+ distribution; a lone scRGB peak is not
   * enough to call a cut because cursors, subtitles, and compositor overshoot can
   * all create one.
   */
  class scene_change_detector_t {
  public:
    bool
    observe(const platf::hdr_frame_luminance_stats_t &stats) {
      if (!detail::has_valid_scene_metrics(stats)) {
        return false;
      }
      if (initialized_ && stats.sample_sequence != 0 && stats.sample_sequence == last_sample_sequence_) {
        return false;
      }

      const bool scene_change = !initialized_ || differs_materially(previous_, stats);
      previous_ = stats;
      last_sample_sequence_ = stats.sample_sequence;
      initialized_ = true;
      return scene_change;
    }

    void
    reset() {
      *this = {};
    }

  private:
    static bool
    differs_materially(
      const platf::hdr_frame_luminance_stats_t &previous,
      const platf::hdr_frame_luminance_stats_t &current) {
      const float mean_delta = std::abs(current.avg_maxrgb_pq - previous.avg_maxrgb_pq);
      const float low_delta = std::abs(current.percentile_10_pq - previous.percentile_10_pq);
      const float high_delta = std::abs(current.percentile_90_pq - previous.percentile_90_pq);

      float distribution_delta_sum = 0.0f;
      float distribution_delta_max = 0.0f;
      size_t distribution_count = 0;
      for (size_t i = 0; i < std::size(current.distribution_maxrgb); ++i) {
        const float previous_nits = previous.distribution_maxrgb[i];
        const float current_nits = current.distribution_maxrgb[i];
        if (!std::isfinite(previous_nits) || !std::isfinite(current_nits) ||
            previous_nits < 0.0f || current_nits < 0.0f) {
          continue;
        }
        const float delta = std::abs(nits_to_pq(current_nits) - nits_to_pq(previous_nits));
        distribution_delta_sum += delta;
        distribution_delta_max = std::max(distribution_delta_max, delta);
        ++distribution_count;
      }
      const float distribution_delta_mean = distribution_count > 0 ?
                                              distribution_delta_sum / distribution_count :
                                              0.0f;

      return mean_delta >= 0.10f ||
             std::max(low_delta, high_delta) >= 0.18f ||
             (distribution_delta_mean >= 0.10f && distribution_delta_max >= 0.14f) ||
             (mean_delta >= 0.06f && distribution_delta_mean >= 0.06f);
    }

    platf::hdr_frame_luminance_stats_t previous_ {};
    uint64_t last_sample_sequence_ = 0;
    bool initialized_ = false;
  };

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
   *
   * All four are PQ-domain statistics, which is why the average comes from
   * stats.avg_maxrgb_pq rather than the linear-light mean beside it: min and max
   * commute with the monotonic PQ transfer function, and the variance is already
   * defined as a difference of PQ percentiles, but the mean does not commute. PQ is
   * concave, so PQ(mean(nits)) >= mean(PQ(nits)), and on a dark frame with small
   * highlights the gap is most of the range — 99% of pixels at 1 nit with 1% at
   * 1000 nits is 0.157 in PQ but PQ(10.99 nits) = 0.303, which would tell a display
   * to anchor its curve twice as high as the picture warrants.
   *
   * A zero avg_maxrgb_pq next to a nonzero linear mean is the signature of an analyzer
   * that never filled the field — the two cannot disagree about whether the frame has
   * any light in it — so that combination yields no metadata rather than a black
   * average. A genuinely black frame reports zero in both and is passed through.
   */
  inline vivid_metadata_t
  vivid_from_stats(const platf::hdr_frame_luminance_stats_t &stats) {
    if (!stats.valid || !std::isfinite(stats.avg_maxrgb_pq) ||
        stats.avg_maxrgb_pq < 0.0f || stats.avg_maxrgb_pq > 1.0f) {
      return {};
    }
    if (stats.avg_maxrgb_pq == 0.0f && !(std::isfinite(stats.avg_maxrgb) && stats.avg_maxrgb <= 0.0f)) {
      return {};
    }

    vivid_metadata_t result;
    result.minimum_maxrgb_pq = pq_to_u12(nits_to_pq(stats.min_maxrgb));
    result.average_maxrgb_pq = pq_to_u12(stats.avg_maxrgb_pq);
    result.variance_maxrgb_pq = pq_to_u12(
      std::max(stats.percentile_90_pq - stats.percentile_10_pq, 0.0f));
    result.maximum_maxrgb_pq = pq_to_u12(nits_to_pq(stats.max_maxrgb));
    result.valid = true;
    return result;
  }

  /**
   * @brief Temporal EMA (Exponential Moving Average) state for HDR luminance stats.
   * Prevents frame-to-frame brightness jitter/flicker in tone mapping by smoothing
   * the raw per-frame GPU statistics over time.
   *
   * This state is owned by the encode session. It must not be shared across sessions:
   * carrying a previous stream's converged luminance into a new one biases the first
   * frames of dynamic metadata until the EMA re-converges.
   *
   * Lives here rather than next to one encoder because both the avcodec and the
   * native NVENC path feed HDR10+ from it. NVENC used to serialize raw analyzer
   * output, so its HDR10+ stepped at every GPU readback while the Vivid metadata
   * beside it was already smoothed by vivid_temporal_filter_t.
   */
  struct hdr_luminance_ema_t {
    float min_maxrgb = 0.0f;
    float max_maxrgb = 0.0f;
    float avg_maxrgb = 0.0f;
    float percentile_99 = 0.0f;
    /// Smoothed maxRGB (nits) at each hdr10plus_percentages entry.
    float distribution_maxrgb[platf::hdr_frame_luminance_stats_t::HDR10PLUS_PERCENTILES] = {};
    bool initialized = false;

    /// EMA smoothing factor: 0.15 = responsive to changes while avoiding flicker.
    /// Lower α = more smoothing (less flicker, slower adaptation).
    /// Scene cuts are handled by fast-tracking when the change exceeds a threshold.
    static constexpr float ALPHA = 0.15f;
    static constexpr float SCENE_CUT_THRESHOLD = 3.0f;  // Ratio threshold for scene cut detection

    /**
     * @brief Apply EMA smoothing to raw per-frame stats.
     * On first frame or scene cuts (>3x luminance change), snaps to current value.
     * Otherwise applies exponential smoothing: smoothed = α·current + (1-α)·previous.
     */
    void
    update(const platf::hdr_frame_luminance_stats_t &raw) {
      if (!raw.valid) return;

      if (!initialized) {
        // First frame: snap to current values
        min_maxrgb = raw.min_maxrgb;
        max_maxrgb = raw.max_maxrgb;
        avg_maxrgb = raw.avg_maxrgb;
        percentile_99 = raw.percentile_99;
        std::copy(std::begin(raw.distribution_maxrgb), std::end(raw.distribution_maxrgb),
          std::begin(distribution_maxrgb));
        initialized = true;
        return;
      }

      // Scene cut detection: if peak luminance changes dramatically, snap immediately
      float ratio = (max_maxrgb > 1.0f) ? raw.max_maxrgb / max_maxrgb : SCENE_CUT_THRESHOLD + 1.0f;
      float alpha = (ratio > SCENE_CUT_THRESHOLD || ratio < 1.0f / SCENE_CUT_THRESHOLD)
                    ? 1.0f  // Scene cut: snap to new values
                    : ALPHA; // Normal: smooth transition

      min_maxrgb = alpha * raw.min_maxrgb + (1.0f - alpha) * min_maxrgb;
      max_maxrgb = alpha * raw.max_maxrgb + (1.0f - alpha) * max_maxrgb;
      avg_maxrgb = alpha * raw.avg_maxrgb + (1.0f - alpha) * avg_maxrgb;
      percentile_99 = alpha * raw.percentile_99 + (1.0f - alpha) * percentile_99;
      for (size_t i = 0; i < std::size(distribution_maxrgb); ++i) {
        distribution_maxrgb[i] =
          alpha * raw.distribution_maxrgb[i] + (1.0f - alpha) * distribution_maxrgb[i];
      }
    }

    void
    reset() {
      *this = {};
    }

    /**
     * @brief raw with the smoothed fields substituted.
     *
     * For callers that hand a whole stats struct to a serializer instead of
     * reading the individual EMA fields. Members this filter does not smooth —
     * analysis_max_nits, sample_sequence, and the PQ-domain statistics HDR Vivid
     * uses, which vivid_temporal_filter_t smooths instead — pass through untouched.
     */
    platf::hdr_frame_luminance_stats_t
    smoothed(const platf::hdr_frame_luminance_stats_t &raw) const {
      if (!initialized) {
        return raw;
      }

      platf::hdr_frame_luminance_stats_t result = raw;
      result.min_maxrgb = min_maxrgb;
      result.max_maxrgb = max_maxrgb;
      result.avg_maxrgb = avg_maxrgb;
      result.percentile_99 = percentile_99;
      std::copy(std::begin(distribution_maxrgb), std::end(distribution_maxrgb),
        std::begin(result.distribution_maxrgb));
      return result;
    }
  };

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

  struct dynamic_metadata_temporal_result_t {
    platf::hdr_frame_luminance_stats_t hdr10plus_stats {};
    vivid_metadata_t vivid {};
  };

  /**
   * Apply the shared scene-aware temporal policy before format-specific
   * serialization. Native encoders and the AVCodec path both own one instance
   * per session so their HDR10+ and HDR Vivid behavior cannot drift apart.
   */
  class dynamic_metadata_temporal_state_t {
  public:
    dynamic_metadata_temporal_result_t
    update(const platf::hdr_frame_luminance_stats_t &stats) {
      if (!detail::has_valid_scene_metrics(stats)) {
        return {};
      }

      const bool scene_change = scene_detector_.observe(stats);
      if (scene_change) {
        hdr10plus_ema_.reset();
        vivid_filter_.reset();
      }

      const auto vivid = vivid_filter_.update(stats);
      hdr10plus_ema_.update(stats);
      return {
        .hdr10plus_stats = hdr10plus_ema_.smoothed(stats),
        .vivid = vivid,
      };
    }

    void
    reset() {
      scene_detector_.reset();
      hdr10plus_ema_.reset();
      vivid_filter_.reset();
    }

  private:
    scene_change_detector_t scene_detector_;
    hdr_luminance_ema_t hdr10plus_ema_;
    vivid_temporal_filter_t vivid_filter_;
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
        std::isfinite(stats.avg_maxrgb_pq) &&
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
             // Vivid's average comes from here, so an analyzer that never filled it
             // must not count as a stable sample the stream can start on.
             stats.avg_maxrgb_pq > 0.0f &&
             stats.avg_maxrgb_pq <= 1.0f &&
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
             // The field Vivid's average is actually built from. A linear-light mean
             // that moves less than 50% can still swing this by a lot down in the dark
             // end, where PQ spends most of its code space.
             std::abs(previous.avg_maxrgb_pq - current.avg_maxrgb_pq) <= 0.15f &&
             std::abs(previous.percentile_10_pq - current.percentile_10_pq) <= 0.15f &&
             std::abs(previous.percentile_90_pq - current.percentile_90_pq) <= 0.15f;
    }

    platf::hdr_frame_luminance_stats_t previous_ {};
    std::array<uint64_t, REQUIRED_SAMPLES> accepted_sequences_ {};
    uint32_t consecutive_samples_ = 0;
    bool ready_ = false;
  };

  /**
   * The stream-startup policy wrapped around vivid_startup_guard_t.
   *
   * The guard decides whether analyzer output is trustworthy; this decides what a
    * session does about it — hold frames back, start temporarily as plain HLG after
    * a wall-clock budget, recover Vivid at an IDR boundary, or emit from the first
    * frame. Both native encoder paths need the identical answer, so the state machine
    * lives here rather than being written twice: NVENC and AMF disagreeing about when
    * HLG starts carrying Vivid would be a bug nobody notices until it is reported as
    * "HDR looks different on my AMD box".
   *
   * The caller owns logging and force-IDR, which is why a transition is reported
   * rather than acted on. A session must feed every capture frame through
   * observe(); skipping frames while holding is what starves the guard of the
   * independent samples it is waiting for.
   */
  class vivid_startup_gate_t {
  public:
    enum class decision_e {
      emit,  ///< Hand this frame's stats to the encoder.
      hold,  ///< Encode nothing yet; keep converting so the analyzer can converge.
      plain_hlg,  ///< Encode without Vivid while a starved analyzer catches up.
      disabled,  ///< This stream cannot emit Vivid at all.
    };

    enum class transition_e {
      none,
      ready,  ///< The guard just converged; the next encoded frame should be an IDR.
      timed_out,  ///< The preroll budget expired; temporarily start as plain HLG.
      recovered,  ///< A timed-out guard converged; switch to Vivid at an IDR.
    };

    struct result_t {
      decision_e decision = decision_e::emit;
      transition_e transition = transition_e::none;
    };

    /**
     * How long a session waits for stable analyzer output before starting without
     * dynamic metadata. The analyzer can take several hundred milliseconds to
     * warm up after a display or encoder reinitialization, so leave enough room
     * for that cold-start path while still bounding the first-frame wait.
     */
    static constexpr auto PREROLL_TIMEOUT = std::chrono::milliseconds { 1500 };

    vivid_startup_gate_t(
      const sunshine_colorspace_t &colorspace,
      int video_format,
      bool analysis_available) {
      if (!colorspace_is_hlg(colorspace)) {
        // PQ carries no plain-HDR10 fallback problem: a mid-stream switch into
        // HDR10+ or Vivid is not visible the way an HLG one is, so there is
        // nothing to wait for.
        state_ = state_e::enabled;
      }
      else if (needs_vivid_startup_preroll(colorspace, video_format, analysis_available)) {
        state_ = state_e::preroll;
      }
      else {
        state_ = state_e::disabled;
      }
    }

    result_t
    observe(
      const platf::hdr_frame_luminance_stats_t &stats,
      std::chrono::steady_clock::time_point now) {
      if (state_ == state_e::disabled) {
        return { decision_e::disabled, transition_e::none };
      }
      if (state_ == state_e::enabled) {
        return { decision_e::emit, transition_e::none };
      }

      if (!preroll_started_) {
        preroll_started_ = now;
      }

      if (guard_.observe(stats)) {
        const bool recovered = state_ == state_e::fallback;
        state_ = state_e::enabled;
        return { decision_e::emit, recovered ? transition_e::recovered : transition_e::ready };
      }
      if (state_ == state_e::fallback) {
        return { decision_e::plain_hlg, transition_e::none };
      }
      if (now - *preroll_started_ >= PREROLL_TIMEOUT) {
        state_ = state_e::fallback;
        return { decision_e::plain_hlg, transition_e::timed_out };
      }
      return { decision_e::hold, transition_e::none };
    }

    /// Whether this session starts by holding frames back, for a startup log line.
    bool
    prerolling() const {
      return state_ == state_e::preroll;
    }

    uint32_t
    consecutive_samples() const {
      return guard_.consecutive_samples();
    }

  private:
    enum class state_e {
      enabled,
      preroll,
      fallback,
      disabled,
    };

    vivid_startup_guard_t guard_;
    std::optional<std::chrono::steady_clock::time_point> preroll_started_;
    state_e state_ = state_e::enabled;
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

  /**
   * Produces this frame's dynamic metadata payloads, owning the temporal state
   * that makes them stable.
   *
   * Both native encoder paths need the same three things done in the same order —
   * update the EMA and the Vivid temporal filter from the raw stats, then
   * serialize whichever formats this stream may carry — and they differ only in
   * how the finished payloads reach the bitstream. Keeping the sequence here means
   * a change to smoothing or to format gating cannot land on one encoder and miss
   * the other.
   *
   * The returned spans point into this object and stay valid until the next
   * build() or reset() call. That is enough for both callers: NVENC hands the
   * pointers straight to nvEncEncodePicture(), and AMF copies them into a spliced
   * bitstream unit, both within the same frame.
   */
  class dynamic_metadata_builder_t {
  public:
    struct payloads_t {
      /// Complete registered T.35 payloads, empty when that format is not emitted.
      std::span<const uint8_t> hdr10plus;
      std::span<const uint8_t> vivid;

      bool
      empty() const {
        return hdr10plus.empty() && vivid.empty();
      }
    };

    /// Which formats this stream may carry, from formats_for().
    void
    configure(formats_t formats) {
      formats_ = formats;
    }

    const formats_t &
    formats() const {
      return formats_;
    }

    /**
     * Update the temporal state from one frame of analyzer output and serialize.
     *
     * The filters are updated even for a format this stream does not emit, so a
     * stream that only carries one of the two still keeps the other's state warm
     * and comparable frame to frame.
     */
    payloads_t
    build(const platf::hdr_frame_luminance_stats_t &stats, uint16_t max_display_luminance) {
      payloads_t payloads;
      if (!stats.valid) {
        return payloads;
      }

      const auto filtered = temporal_state_.update(stats);

      if (formats_.hdr10plus) {
        const auto size = serialize_hdr10plus_t35(
          filtered.hdr10plus_stats, max_display_luminance, hdr10plus_storage_);
        if (size > 0) {
          payloads.hdr10plus = std::span(hdr10plus_storage_).first(size);
        }
      }
      if (formats_.vivid && serialize_vivid_t35(filtered.vivid, vivid_storage_) > 0) {
        payloads.vivid = vivid_storage_;
      }
      return payloads;
    }

    /// Drop temporal history, e.g. when an encoder is recreated mid-session.
    void
    reset() {
      temporal_state_.reset();
      vivid_storage_.clear();
    }

  private:
    formats_t formats_ {};
    dynamic_metadata_temporal_state_t temporal_state_;
    std::array<uint8_t, hdr10plus_t35_max_payload_size> hdr10plus_storage_ {};
    std::vector<uint8_t> vivid_storage_;
  };

}  // namespace video::hdr_metadata
