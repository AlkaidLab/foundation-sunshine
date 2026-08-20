/**
 * @file src/video_hdr_metadata.cpp
 * @brief HDR dynamic metadata serializers backed by FFmpeg.
 */
#include "video_hdr_metadata.h"

#include <array>

extern "C" {
#include <libavutil/hdr_dynamic_metadata.h>
#include <libavutil/rational.h>
}

namespace video::hdr_metadata {

  namespace {

    constexpr std::array<uint8_t, hdr10plus_t35_prefix_size> hdr10plus_t35_prefix {
      0xB5,  // itu_t_t35_country_code: United States
      0x00, 0x3C,  // terminal_provider_code: SMPTE
      0x00, 0x01,  // terminal_provider_oriented_code: HDR10+
      0x04,  // application_identifier: SMPTE ST 2094-40
    };

    bool
    rational_equals(const AVRational &lhs, const AVRational &rhs) {
      return av_cmp_q(lhs, rhs) == 0;
    }

  }  // namespace

  size_t
  serialize_hdr10plus_t35(const platf::hdr_frame_luminance_stats_t &stats,
    uint16_t max_display_luminance,
    std::span<uint8_t> payload) {
    if (!stats.valid || payload.size() < hdr10plus_t35_prefix_size + AV_HDR_PLUS_MAX_PAYLOAD_SIZE) {
      return 0;
    }

    // The 99th percentile is what maxSCL reports; see hdr10plus_from_luminance().
    const auto frame_metadata = hdr10plus_from_luminance(
      stats.percentile_99, stats.avg_maxrgb, max_display_luminance, stats.distribution_maxrgb);
    if (!frame_metadata.valid) {
      return 0;
    }

    AVDynamicHDRPlus metadata {};
    metadata.itu_t_t35_country_code = hdr10plus_t35_prefix[0];
    // FFmpeg serializes ST 2094-40 as Application Version 1.
    metadata.application_version = hdr10plus_application_version;
    metadata.num_windows = 1;
    metadata.targeted_system_display_maximum_luminance = av_make_q(
      frame_metadata.targeted_system_display_maximum_luminance, 1);
    metadata.targeted_system_display_actual_peak_luminance_flag = 0;
    metadata.mastering_display_actual_peak_luminance_flag = 0;

    auto &params = metadata.params[0];
    params.window_upper_left_corner_x = av_make_q(0, 1);
    params.window_upper_left_corner_y = av_make_q(0, 1);
    params.window_lower_right_corner_x = av_make_q(1, 1);
    params.window_lower_right_corner_y = av_make_q(1, 1);

    const AVRational maxscl = av_make_q(
      frame_metadata.maxscl, hdr10plus_normalized_scale);
    params.maxscl[0] = maxscl;
    params.maxscl[1] = maxscl;
    params.maxscl[2] = maxscl;
    params.average_maxrgb = av_make_q(
      frame_metadata.average_maxrgb, hdr10plus_normalized_scale);
    // Deployment profiles always carry these nine percentiles. A zero count parses
    // back cleanly through FFmpeg but is not what any shipping HDR10+ stream sends.
    params.num_distribution_maxrgb_percentiles =
      static_cast<uint8_t>(hdr10plus_percentages.size());
    for (size_t i = 0; i < hdr10plus_percentages.size(); ++i) {
      params.distribution_maxrgb[i].percentage = hdr10plus_percentages[i];
      params.distribution_maxrgb[i].percentile = av_make_q(
        frame_metadata.distribution_maxrgb[i], hdr10plus_normalized_scale);
    }
    params.fraction_bright_pixels = av_make_q(0, 1);
    params.tone_mapping_flag = 0;
    params.color_saturation_mapping_flag = 0;

    std::array<uint8_t, AV_HDR_PLUS_MAX_PAYLOAD_SIZE> body_storage {};
    uint8_t *body = body_storage.data();
    size_t body_size = body_storage.size();
    if (av_dynamic_hdr_plus_to_t35(&metadata, &body, &body_size) < 0 ||
        body != body_storage.data() || body_size == 0 || body_size > body_storage.size()) {
      return 0;
    }

    // FFmpeg omits this 48-bit registration prefix. NVENC adds only the
    // surrounding HEVC SEI or AV1 metadata OBU syntax.
    std::copy(hdr10plus_t35_prefix.begin(), hdr10plus_t35_prefix.end(), payload.begin());
    std::copy_n(body, body_size, payload.begin() + hdr10plus_t35_prefix.size());

    AVDynamicHDRPlus decoded {};
    bool valid_round_trip =
      av_dynamic_hdr_plus_from_t35(&decoded, body, body_size) >= 0 &&
      decoded.application_version == metadata.application_version &&
      decoded.num_windows == metadata.num_windows &&
      rational_equals(decoded.targeted_system_display_maximum_luminance,
        metadata.targeted_system_display_maximum_luminance) &&
      rational_equals(decoded.params[0].maxscl[0], params.maxscl[0]) &&
      rational_equals(decoded.params[0].maxscl[1], params.maxscl[1]) &&
      rational_equals(decoded.params[0].maxscl[2], params.maxscl[2]) &&
      rational_equals(decoded.params[0].average_maxrgb, params.average_maxrgb) &&
      decoded.params[0].num_distribution_maxrgb_percentiles ==
        params.num_distribution_maxrgb_percentiles;

    // Checking the distribution too: leaving it out is what let a non-conformant
    // empty percentile list ship while the round trip still reported success.
    for (size_t i = 0; valid_round_trip && i < hdr10plus_percentages.size(); ++i) {
      valid_round_trip =
        decoded.params[0].distribution_maxrgb[i].percentage ==
          params.distribution_maxrgb[i].percentage &&
        rational_equals(decoded.params[0].distribution_maxrgb[i].percentile,
          params.distribution_maxrgb[i].percentile);
    }

    if (!valid_round_trip) {
      return 0;
    }

    return hdr10plus_t35_prefix.size() + body_size;
  }

}  // namespace video::hdr_metadata
