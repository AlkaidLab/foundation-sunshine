/**
 * @file src/stream_quality.cpp
 * @brief Low-bitrate screen clarity planning helpers.
 */
#include "stream_quality.h"

#include <algorithm>
#include <cmath>

namespace stream_quality {
  namespace {
    double
    target_bpp_for_codec(int video_format, int chroma_sampling_type, content_type_e content_type) {
      double target = 0.018;
      if (video_format == 1) {
        target = 0.014;
      }
      else if (video_format == 2) {
        target = 0.012;
      }

      if (chroma_sampling_type == 1) {
        target *= 1.2;
      }

      switch (content_type) {
        case content_type_e::text:
          target *= 1.28;
          break;
        case content_type_e::motion:
          target *= 1.0;
          break;
        case content_type_e::game:
          target *= 1.0;
          break;
        case content_type_e::desktop:
        default:
          break;
      }

      return target;
    }

    int
    minimum_responsive_fps(int video_bitrate_kbps, int requested_fps, content_type_e content_type) {
      if (requested_fps >= 90 &&
          video_bitrate_kbps >= 2200 &&
          content_type != content_type_e::text) {
        return video_bitrate_kbps >= 4500 &&
                   (content_type == content_type_e::motion ||
                    content_type == content_type_e::game) ?
                 72 :
                 60;
      }

      if (content_type == content_type_e::motion || content_type == content_type_e::game) {
        if (video_bitrate_kbps < 1500) {
          return 24;
        }
        if (video_bitrate_kbps < 3000) {
          return 30;
        }
        return 45;
      }

      if (video_bitrate_kbps < 1500) {
        return 18;
      }
      if (video_bitrate_kbps < 3000) {
        return 24;
      }
      return 30;
    }

    int
    target_qp_for_budget(double bpp, content_type_e content_type) {
      int qp = bpp < 0.008 ? 34 : bpp < 0.012 ? 30 : bpp < 0.018 ? 26 : 22;
      if (content_type == content_type_e::text) {
        qp = std::max(20, qp - 2);
      }
      else if (content_type == content_type_e::motion || content_type == content_type_e::game) {
        qp = std::min(38, qp + 1);
      }
      return qp;
    }
  }  // namespace

  double
  bits_per_pixel_per_frame(const stream_description_t &stream) {
    if (stream.width <= 0 ||
        stream.height <= 0 ||
        stream.fps <= 0 ||
        stream.video_bitrate_kbps <= 0) {
      return 0.0;
    }

    const auto pixels_per_frame = static_cast<double>(stream.width) *
                                  static_cast<double>(stream.height);
    return static_cast<double>(stream.video_bitrate_kbps) * 1000.0 /
           (pixels_per_frame * static_cast<double>(stream.fps));
  }

  clarity_plan_t
  plan_low_bitrate_clarity(const stream_description_t &stream) {
    clarity_plan_t plan {
      .enabled = false,
      .video_bitrate_kbps = stream.video_bitrate_kbps,
      .effective_fps = stream.fps,
      .effective_chroma_sampling_type = stream.chroma_sampling_type,
      .bits_per_pixel_per_frame = bits_per_pixel_per_frame(stream),
      .target_qp = 0,
      .roi_enabled = false,
      .dirty_region_priority = false,
      .prefer_temporal_layers = false,
      .discardable_enhancement_layer = false,
      .prefer_long_term_reference = false,
      .prefer_intra_refresh = false,
      .intent_flags = 0,
      .sharpen_alpha = 0.0f,
    };

    if (stream.width <= 0 ||
        stream.height <= 0 ||
        stream.fps <= 0 ||
        stream.video_bitrate_kbps <= 0) {
      return plan;
    }

    auto effective_stream = stream;
    const auto requested_target_bpp = target_bpp_for_codec(stream.video_format, stream.chroma_sampling_type, stream.content_type);
    if (stream.chroma_sampling_type == 1 &&
        plan.bits_per_pixel_per_frame < requested_target_bpp * 1.5) {
      effective_stream.chroma_sampling_type = 0;
      plan.effective_chroma_sampling_type = 0;
    }

    const auto target_bpp = target_bpp_for_codec(effective_stream.video_format,
                                                 effective_stream.chroma_sampling_type,
                                                 effective_stream.content_type);
    const auto pixels_per_frame = static_cast<double>(effective_stream.width) *
                                  static_cast<double>(effective_stream.height);
    const auto clarity_fps = static_cast<int>(std::lround(
      static_cast<double>(effective_stream.video_bitrate_kbps) * 1000.0 /
      (pixels_per_frame * target_bpp)));

    const auto fps_floor = minimum_responsive_fps(effective_stream.video_bitrate_kbps,
                                                  effective_stream.fps,
                                                  effective_stream.content_type);
    plan.effective_fps = std::clamp(clarity_fps, std::min(fps_floor, stream.fps), stream.fps);
    plan.target_qp = target_qp_for_budget(plan.bits_per_pixel_per_frame, stream.content_type);
    const bool interest_pressure = plan.bits_per_pixel_per_frame < target_bpp * 1.15;
    const bool strong_interest_pressure = plan.bits_per_pixel_per_frame < target_bpp;
    plan.roi_enabled = stream.content_type == content_type_e::text ||
                       strong_interest_pressure;
    plan.dirty_region_priority = plan.roi_enabled || interest_pressure;
    plan.prefer_temporal_layers = stream.fps >= 90 &&
                                  interest_pressure;
    plan.discardable_enhancement_layer = plan.prefer_temporal_layers &&
                                         stream.video_format != 0;
    plan.prefer_long_term_reference = stream.content_type == content_type_e::text ||
                                      stream.content_type == content_type_e::desktop;
    plan.prefer_intra_refresh = stream.content_type == content_type_e::motion ||
                                stream.content_type == content_type_e::game;
    if (plan.roi_enabled) {
      plan.intent_flags |= clarity_intent_roi;
    }
    if (plan.dirty_region_priority) {
      plan.intent_flags |= clarity_intent_dirty_region;
    }
    if (plan.prefer_temporal_layers) {
      plan.intent_flags |= clarity_intent_temporal_layers;
    }
    if (plan.discardable_enhancement_layer) {
      plan.intent_flags |= clarity_intent_discardable_enhancement;
    }
    if (plan.prefer_long_term_reference) {
      plan.intent_flags |= clarity_intent_long_term_reference;
    }
    if (plan.prefer_intra_refresh) {
      plan.intent_flags |= clarity_intent_intra_refresh;
    }
    if (plan.bits_per_pixel_per_frame < target_bpp) {
      plan.sharpen_alpha = stream.content_type == content_type_e::text ? 0.22f :
                           stream.content_type == content_type_e::desktop ? 0.14f :
                           0.06f;
    }
    plan.enabled = plan.effective_fps < stream.fps ||
                   plan.effective_chroma_sampling_type != stream.chroma_sampling_type ||
                   plan.roi_enabled ||
                   plan.dirty_region_priority ||
                   plan.prefer_temporal_layers ||
                   plan.prefer_intra_refresh ||
                   plan.sharpen_alpha > 0.0f;
    return plan;
  }

  int
  startup_bitrate_for_ceiling(const stream_description_t &stream) {
    if (stream.video_bitrate_kbps <= 0 ||
        stream.width <= 0 ||
        stream.height <= 0 ||
        stream.fps <= 0) {
      return stream.video_bitrate_kbps;
    }

    constexpr int high_ceiling_threshold_kbps = 30000;
    constexpr int min_safe_startup_kbps = 8000;
    constexpr int max_safe_startup_kbps = 30000;
    const auto pixels_per_second = static_cast<double>(stream.width) *
                                   static_cast<double>(stream.height) *
                                   static_cast<double>(stream.fps);
    const bool high_pixel_rate = pixels_per_second >= 500'000'000.0 ||
                                 (stream.width * stream.height >= 4'500'000 && stream.fps >= 90);
    if (stream.video_bitrate_kbps <= high_ceiling_threshold_kbps && !high_pixel_rate) {
      return stream.video_bitrate_kbps;
    }

    const auto startup_from_pixels = static_cast<int>(std::lround(pixels_per_second * 0.018 / 1000.0));
    return std::min(stream.video_bitrate_kbps,
                    std::clamp(startup_from_pixels, min_safe_startup_kbps, max_safe_startup_kbps));
  }

  int
  startup_fps_for_bitrate(const stream_description_t &stream, int startup_bitrate_kbps) {
    if (startup_bitrate_kbps <= 0 ||
        stream.width <= 0 ||
        stream.height <= 0 ||
        stream.fps <= 0) {
      return stream.fps;
    }

    if (stream.fps <= 60) {
      return stream.fps;
    }

    const auto pixels_per_frame = static_cast<double>(stream.width) *
                                  static_cast<double>(stream.height);
    const auto startup_target_bpp = target_bpp_for_codec(stream.video_format,
                                                         stream.chroma_sampling_type,
                                                         stream.content_type) * 1.6;
    auto fps_from_budget = static_cast<int>(std::floor(
      static_cast<double>(startup_bitrate_kbps) * 1000.0 /
      (pixels_per_frame * startup_target_bpp)));

    const auto min_interactive_fps = startup_bitrate_kbps >= 2500 ? 60 :
                                     startup_bitrate_kbps >= 1500 ? 45 :
                                     30;
    return std::clamp(fps_from_budget, std::min(min_interactive_fps, stream.fps), stream.fps);
  }

  int
  static_frame_keepalive_fps(int requested_fps, bool variable_refresh_rate, int minimum_fps_target) {
    if (requested_fps <= 0) {
      return 1;
    }

    if (minimum_fps_target > 0) {
      return std::clamp(minimum_fps_target, 1, requested_fps);
    }

    if (variable_refresh_rate) {
      const auto keepalive = static_cast<int>(std::ceil(static_cast<double>(requested_fps) / 4.0));
      return std::clamp(keepalive, 5, std::min(30, requested_fps));
    }

    const auto legacy_minimum = static_cast<int>(std::ceil(static_cast<double>(requested_fps) / 2.0));
    return std::clamp(legacy_minimum, 1, requested_fps);
  }
}  // namespace stream_quality
