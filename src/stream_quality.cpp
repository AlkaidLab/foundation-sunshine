/**
 * @file src/stream_quality.cpp
 * @brief Low-bitrate screen clarity planning helpers.
 */
#include "stream_quality.h"

#include <algorithm>
#include <cmath>

namespace stream_quality {
  namespace {
    // Static keepalive frames are duplicates/re-converts, not freshly captured
    // desktop frames.  They keep VRR clients responsive during drag/held-button
    // interaction, but flooding them at a 120/150 Hz client target can occupy
    // the 4K encoder/packet path and make real frames arrive late.  Real new
    // captured frames still use the requested stream FPS; this cap applies only
    // to duplicate static keepalives.
    constexpr int kInteractiveStaticKeepaliveFpsCap = 60;
    constexpr int kInteractiveStaticKeepaliveWeakFpsCap = 30;
    constexpr int kInteractiveStaticKeepaliveWeakThresholdFps = 90;

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
        if (video_bitrate_kbps >= 4500 &&
            (content_type == content_type_e::motion ||
             content_type == content_type_e::game)) {
          return 72;
        }

        // For remote-desktop/game streaming, a cursor/scroll/drag workload that
        // falls straight to 60fps feels worse than a moderately softer but
        // temporally stable stream.  Keep the floor proportional to the user's
        // target instead of sticky-60; stream-quality can still step down linearly
        // when render/network pressure proves the route cannot hold it.
        return std::clamp(static_cast<int>(std::lround(static_cast<double>(requested_fps) * 0.70)),
                          60,
                          requested_fps);
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

    auto target_bpp = target_bpp_for_codec(effective_stream.video_format,
                                           effective_stream.chroma_sampling_type,
                                           effective_stream.content_type);
    if (effective_stream.fps >= 90 &&
        effective_stream.content_type != content_type_e::text) {
      // OBS-like low-latency tradeoff: under high-refresh motion pressure, bias
      // toward cadence first and rely on AQ/ROI/dirty fallback for local detail.
      // Do not add buffering or ABR segments.
      target_bpp *= 0.82;
    }
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
    constexpr int min_safe_startup_kbps = 6000;
    constexpr int max_safe_startup_kbps = 18000;
    constexpr int max_high_pixel_startup_kbps = 70000;
    const auto pixels_per_second = static_cast<double>(stream.width) *
                                   static_cast<double>(stream.height) *
                                   static_cast<double>(stream.fps);
    const bool high_pixel_rate = pixels_per_second >= 500'000'000.0 ||
                                 (stream.width * stream.height >= 4'500'000 && stream.fps >= 90);
    if (stream.video_bitrate_kbps <= high_ceiling_threshold_kbps && !high_pixel_rate) {
      return stream.video_bitrate_kbps;
    }

    /*
     * High-refresh 4K streams cannot start from the same 18 Mbps cap used for
     * unknown routes.  At 3840x2160@150, that is below a readable first-screen
     * budget and creates a blurry/frozen startup before feedback has a chance
     * to prove whether the route is actually weak.  Seed high-pixel streams at
     * a conservative but viewable bpp, then let stream-quality ramp down if delivery
     * evidence says the path cannot sustain it.
     */
    const auto startup_bpp = high_pixel_rate ?
                               target_bpp_for_codec(stream.video_format,
                                                    stream.chroma_sampling_type,
                                                    stream.content_type) * 2.6 :
                               0.015;
    const auto startup_from_pixels = static_cast<int>(std::lround(pixels_per_second * startup_bpp / 1000.0));
    const auto startup_cap = high_pixel_rate ? max_high_pixel_startup_kbps : max_safe_startup_kbps;
    return std::min(stream.video_bitrate_kbps,
                    std::clamp(startup_from_pixels, min_safe_startup_kbps, startup_cap));
  }

  int
  startup_bitrate_preserving_seed(const stream_description_t &stream, int seeded_bitrate_kbps) {
    const auto computed_startup = startup_bitrate_for_ceiling(stream);
    if (seeded_bitrate_kbps <= 0) {
      return computed_startup;
    }

    // RTSP may already have chosen a lower remote-safe startup point from the
    // user's quality ceiling. The stream runtime may reduce that further, but
    // must never raise it using the high-pixel "ideal demand" estimate.
    return std::min(seeded_bitrate_kbps, computed_startup);
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
                                                         stream.content_type) *
                                    1.35;
    const auto fps_from_budget = static_cast<int>(std::floor(
      static_cast<double>(startup_bitrate_kbps) * 1000.0 /
      (pixels_per_frame * startup_target_bpp)));

    const auto high_refresh_floor = std::clamp(
      static_cast<int>(std::lround(static_cast<double>(stream.fps) * 0.80)),
      72,
      stream.fps);
    return std::clamp(fps_from_budget, high_refresh_floor, stream.fps);
  }

  int
  static_frame_keepalive_fps(int requested_fps,
                             bool variable_refresh_rate,
                             int minimum_fps_target,
                             static_frame_mode_e mode) {
    if (requested_fps <= 0) {
      return 1;
    }

    if (minimum_fps_target > 0) {
      return std::clamp(minimum_fps_target, 1, requested_fps);
    }

    if (variable_refresh_rate) {
      if (mode == static_frame_mode_e::interactive_input) {
        const auto cap = requested_fps <= kInteractiveStaticKeepaliveWeakThresholdFps ?
                           kInteractiveStaticKeepaliveWeakFpsCap :
                           kInteractiveStaticKeepaliveFpsCap;
        return std::clamp(cap, 1, requested_fps);
      }
      return 1;
    }

    const auto non_vrr_idle_keepalive = std::min(5, requested_fps);
    return std::clamp(non_vrr_idle_keepalive, 1, requested_fps);
  }

  static_frame_mode_e
  static_frame_mode_for_input_activity(bool input_active, bool cursor_plane_active) {
    (void) cursor_plane_active;
    if (!input_active) {
      return static_frame_mode_e::idle;
    }
    return static_frame_mode_e::interactive_input;
  }

  bool
  input_activity_is_recent(bool recent_control_input,
                           bool drag_active,
                           bool mouse_button_active,
                           bool gamepad_active) {
    return recent_control_input || drag_active || mouse_button_active || gamepad_active;
  }
}  // namespace stream_quality
