#include "weak_net_controller.h"

#include <algorithm>
#include <cmath>

namespace weak_net {
  namespace {
    double
    ratio(std::uint32_t numerator, std::uint32_t denominator) {
      if (denominator == 0) {
        return 0.0;
      }
      return static_cast<double>(numerator) / static_cast<double>(denominator);
    }

    double
    ewma(double previous, double sample, double alpha) {
      return previous == 0.0 ? sample : previous * (1.0 - alpha) + sample * alpha;
    }

    double
    clamp01(double value) {
      return std::clamp(value, 0.0, 1.0);
    }

    std::uint32_t
    feedback_frame_sample_count(const feedback_t &feedback) {
      return std::max({
        feedback.frames_seen,
        feedback.complete_frames + feedback.recovered_frames + feedback.unrecoverable_frames,
        feedback.complete_frames + feedback.unrecoverable_frames,
        feedback.recovered_frames + feedback.unrecoverable_frames,
        feedback.displayed_frames + feedback.unrecoverable_frames,
        1U,
      });
    }

    int
    clamp_percent(int value, int max_fec_percentage = controller_t::max_fec_percentage) {
      return std::clamp(value, 0, std::clamp(max_fec_percentage, 0, controller_t::max_fec_percentage));
    }

    int
    clamp_fps(int value, int min_fps, int baseline_fps) {
      min_fps = std::clamp(min_fps, 1, std::max(1, baseline_fps));
      return std::clamp(value, min_fps, std::max(min_fps, baseline_fps));
    }

    int
    approach_int(int current, int target, double fraction, int min_step, int max_step) {
      if (current == target) {
        return current;
      }

      const auto delta = target - current;
      auto step = static_cast<int>(std::lround(std::abs(delta) * fraction));
      step = std::clamp(step, min_step, std::max(min_step, max_step));
      step = std::min(step, std::abs(delta));
      return current + (delta > 0 ? step : -step);
    }

    int
    clamp_delta_int(int previous, int target, int max_down_step, int max_up_step) {
      if (target < previous) {
        return std::max(target, previous - std::max(0, max_down_step));
      }
      if (target > previous) {
        return std::min(target, previous + std::max(0, max_up_step));
      }
      return target;
    }

    int
    proportional_step(int value, double fraction, int min_step, int max_step) {
      const auto step = static_cast<int>(std::lround(static_cast<double>(std::max(1, value)) * fraction));
      return std::clamp(step, min_step, std::max(min_step, max_step));
    }

    int
    gentle_probe_step(int current_bitrate_kbps, int ceiling_bitrate_kbps) {
      const auto remaining = std::max(0, ceiling_bitrate_kbps - current_bitrate_kbps);
      if (remaining == 0) {
        return 0;
      }
      return std::clamp(static_cast<int>(std::lround(remaining * 0.05)), 500, 5000);
    }

    int
    total_bitrate_for_encoding_bitrate(int encoding_bitrate_kbps, int fec_percentage) {
      if (encoding_bitrate_kbps <= 0) {
        return encoding_bitrate_kbps;
      }

      fec_percentage = clamp_percent(fec_percentage);
      return fec_percentage > 0 ?
               static_cast<int>(std::lround(
                 static_cast<double>(encoding_bitrate_kbps) *
                 static_cast<double>(100 + fec_percentage) / 100.0)) :
               encoding_bitrate_kbps;
    }

    int
    encoding_bitrate_for_total_budget(int total_bitrate_kbps, int fec_percentage) {
      if (total_bitrate_kbps <= 0) {
        return total_bitrate_kbps;
      }

      fec_percentage = clamp_percent(fec_percentage);
      return fec_percentage > 0 ?
               static_cast<int>(std::lround(
                 static_cast<double>(total_bitrate_kbps) *
                 100.0 / static_cast<double>(100 + fec_percentage))) :
               total_bitrate_kbps;
    }

    int
    pacing_budget_for_encoding_bitrate(int encoding_bitrate_kbps, int fec_percentage) {
      if (encoding_bitrate_kbps <= 0) {
        return encoding_bitrate_kbps;
      }

      const auto total_bitrate_kbps = total_bitrate_for_encoding_bitrate(encoding_bitrate_kbps, fec_percentage);
      return static_cast<int>(std::lround(static_cast<double>(total_bitrate_kbps) * 1.04));
    }

    int
    clamp_pacing_budget(int pacing_bitrate_kbps, int ceiling_total_bitrate_kbps, int min_bitrate_kbps) {
      if (ceiling_total_bitrate_kbps > 0) {
        pacing_bitrate_kbps = std::min(pacing_bitrate_kbps, ceiling_total_bitrate_kbps);
      }
      return std::max(std::min(min_bitrate_kbps, std::max(1, ceiling_total_bitrate_kbps)), pacing_bitrate_kbps);
    }

    int
    linear_fec_target_for_random_loss(int baseline_fec_percentage,
                                      int max_fec_percentage,
                                      double packet_loss,
                                      double recovered_frames,
                                      double unrecoverable_frames) {
      auto random_loss_signal = std::max({
        packet_loss * 1.4,
        recovered_frames * 1.15,
        unrecoverable_frames * 3.0,
      });
      if (unrecoverable_frames >= 0.25) {
        random_loss_signal = 1.0;
      }
      else if (unrecoverable_frames >= 0.10) {
        random_loss_signal = std::max(random_loss_signal, 0.70);
      }
      else if (unrecoverable_frames >= 0.03) {
        random_loss_signal = std::max(random_loss_signal, 0.45);
      }
      else if (recovered_frames >= 0.20 && unrecoverable_frames < 0.005) {
        random_loss_signal = std::max(random_loss_signal, 0.30);
      }
      const auto fec_headroom = static_cast<int>(std::ceil(random_loss_signal * 100.0));
      return clamp_percent(baseline_fec_percentage + fec_headroom, max_fec_percentage);
    }

    int
    cap_fec_for_observed_efficiency(int target_fec,
                                    int baseline_fec_percentage,
                                    double fec_efficiency,
                                    double unrecoverable_frames,
                                    double recovered_frames) {
      if (unrecoverable_frames >= 0.08 && fec_efficiency < 0.10) {
        return std::min(target_fec, baseline_fec_percentage + 15);
      }
      if (unrecoverable_frames >= 0.03 && fec_efficiency < 0.20) {
        return std::min(target_fec, baseline_fec_percentage + 20);
      }
      if (unrecoverable_frames < 0.005 && fec_efficiency >= 0.90) {
        return std::min(target_fec, baseline_fec_percentage + 30);
      }
      if (unrecoverable_frames >= 0.08 && fec_efficiency < 0.45) {
        return std::min(target_fec, std::max(baseline_fec_percentage + 20, 35));
      }
      if (unrecoverable_frames >= 0.03 && fec_efficiency < 0.60) {
        return std::min(target_fec, std::max(baseline_fec_percentage + 35, 50));
      }
      if (recovered_frames >= 0.05 && unrecoverable_frames < 0.005 && fec_efficiency >= 0.92) {
        return std::min(target_fec, std::max(baseline_fec_percentage + 45, 60));
      }
      return target_fec;
    }

    int
    readable_interactive_floor_kbps(const config_t &config, int encoding_ceiling_kbps) {
      const auto ceiling = std::max(config.min_bitrate_kbps, encoding_ceiling_kbps);
      const auto floor_fraction = config.baseline_fps >= 90 ? 0.40 : 0.35;
      const auto floor = std::max(config.min_bitrate_kbps,
                                  static_cast<int>(std::lround(static_cast<double>(config.baseline_bitrate_kbps) *
                                                               floor_fraction)));
      return std::min(ceiling, floor);
    }

    int
    user_quality_budget_kbps(const config_t &config) {
      return std::max(1, config.user_quality_kbps > 0 ? config.user_quality_kbps : config.baseline_bitrate_kbps);
    }

    int
    ideal_demand_budget_kbps(const config_t &config) {
      return std::max(user_quality_budget_kbps(config),
                      config.ideal_demand_kbps > 0 ? config.ideal_demand_kbps : config.baseline_bitrate_kbps);
    }

    int
    fps_protection_budget_kbps(const config_t &config) {
      return std::max(user_quality_budget_kbps(config),
                      config.fps_needed_kbps > 0 ? config.fps_needed_kbps : user_quality_budget_kbps(config));
    }

    int
    configured_encoding_ceiling_kbps(const config_t &config, int fec_percentage) {
      const auto total_limited_encoding = encoding_bitrate_for_total_budget(config.ceiling_total_bitrate_kbps,
                                                                           fec_percentage);
      const auto requested_runtime_ceiling = std::min(
        std::max(user_quality_budget_kbps(config), fps_protection_budget_kbps(config)),
        ideal_demand_budget_kbps(config));
      return std::min(requested_runtime_ceiling, total_limited_encoding);
    }

    int
    high_refresh_emergency_floor(int baseline_fps) {
      return baseline_fps >= 90 ? 60 : 1;
    }

    int
    high_refresh_interactive_floor(int baseline_fps) {
      return baseline_fps >= 90 ? 72 : high_refresh_emergency_floor(baseline_fps);
    }

    double
    linear_audio_bitrate_scale(double audio_pressure, bool audio_crisis) {
      const auto bounded_pressure = std::clamp(audio_pressure, 0.0, 0.5);
      const auto max_reduction = audio_crisis ? 0.04 : 0.025;
      const auto reduction = std::clamp(0.01 + bounded_pressure * 0.075, 0.012, max_reduction);
      return 1.0 - reduction;
    }

    double
    audio_continuity_pressure(const feedback_t &feedback) {
      const auto duration_ms = std::max(feedback.duration_ms, 1U);
      const auto event_count = std::max({
        feedback_frame_sample_count(feedback),
        duration_ms / 10U,
        1U,
      });
      const auto drop_ms = feedback.late_audio_drops * 5U;
      const auto concealment_ms = std::max(feedback.audio_concealed_ms,
                                           feedback.audio_plc_ms + feedback.audio_fade_ms) +
                                  drop_ms;
      const auto concealment_pressure = ratio(concealment_ms, duration_ms);
      const auto drop_pressure = ratio(feedback.late_audio_drops,
                                       event_count);
      const auto underrun_pressure = ratio(feedback.audio_underruns,
                                           event_count);
      const auto buffer_pressure = feedback.audio_buffer_depth_ms > 120 ?
                                     static_cast<double>(feedback.audio_buffer_depth_ms - 120) / 240.0 :
                                     0.0;

      return clamp01(std::max({ underrun_pressure, concealment_pressure, drop_pressure, buffer_pressure }));
    }

    int
    observed_video_bitrate_kbps(const feedback_t &feedback) {
      if (feedback.duration_ms == 0 || feedback.video_bytes == 0) {
        return 0;
      }

      const auto bits = static_cast<double>(feedback.video_bytes) * 8.0;
      return static_cast<int>(std::lround(bits / static_cast<double>(feedback.duration_ms)));
    }

    std::uint64_t
    configured_frame_area(const config_t &config) {
      if (config.frame_width <= 0 || config.frame_height <= 0) {
        return 0;
      }

      return static_cast<std::uint64_t>(config.frame_width) *
             static_cast<std::uint64_t>(config.frame_height);
    }

    double
    bits_per_pixel_per_frame(int bitrate_kbps, int fps, std::uint64_t frame_area) {
      if (bitrate_kbps <= 0 || fps <= 0 || frame_area == 0) {
        return 0.0;
      }

      return static_cast<double>(bitrate_kbps) * 1000.0 /
             (static_cast<double>(frame_area) * static_cast<double>(fps));
    }

    double
    readable_bpp_target(int chroma_sampling_type) {
      return chroma_sampling_type == 1 ? 0.070 : 0.052;
    }

    bool
    is_static_idle_without_video_samples(const feedback_t &feedback) {
      return feedback.frames_seen == 0 &&
             feedback.complete_frames == 0 &&
             feedback.recovered_frames == 0 &&
             feedback.unrecoverable_frames == 0 &&
             feedback.displayed_frames == 0 &&
             feedback.missing_packets == 0 &&
             feedback.total_packets == 0 &&
             feedback.received_packets == 0 &&
             feedback.video_bytes == 0 &&
             feedback.dirty_area == 0 &&
             !feedback.full_frame_dirty &&
             feedback.rfi_requests == 0 &&
             feedback.waiting_for_rfi_frames == 0 &&
             feedback.late_frames == 0 &&
             feedback.decode_queue_depth == 0 &&
             feedback.render_queue_depth == 0 &&
             feedback.input_queue_depth == 0 &&
             feedback.input_send_latency_us == 0 &&
             feedback.input_ack_latency_us == 0;
    }

    int
    scale_target_for_pressure(double motion_pressure, double burst_pressure, double bpp_pressure, bool input_active) {
      const auto combined = std::max({ motion_pressure, burst_pressure, bpp_pressure });
      if (burst_pressure >= 0.95 || (motion_pressure >= 0.95 && bpp_pressure >= 0.70)) {
        return input_active ? 67 : 60;
      }
      if (combined >= 0.78 || (motion_pressure >= 0.80 && bpp_pressure >= 0.48)) {
        return 75;
      }
      if (combined >= 0.50 || (motion_pressure >= 0.65 && bpp_pressure >= 0.35)) {
        return 85;
      }
      return 100;
    }

    int
    tier_for_scale(int scale_percent) {
      if (scale_percent <= 60) {
        return 4;
      }
      if (scale_percent <= 67) {
        return 3;
      }
      if (scale_percent <= 75) {
        return 2;
      }
      if (scale_percent <= 85) {
        return 1;
      }
      return 0;
    }

    int
    fps_target_for_profile_fallback(int baseline_fps, int current_fps, double bpp_pressure, bool input_active) {
      if (baseline_fps < 90 || !input_active || bpp_pressure < 0.30) {
        return current_fps;
      }

      const auto target = bpp_pressure >= 0.72 ? 72 :
                          bpp_pressure >= 0.52 ? 90 :
                          105;
      return std::min(current_fps, std::clamp(target, 72, baseline_fps));
    }
  }  // namespace

  void
  controller_t::configure(config_t config) {
    config_ = config;
    config_.min_bitrate_kbps = std::max(config_.min_bitrate_kbps, 1);
    config_.baseline_bitrate_kbps = std::max(config_.baseline_bitrate_kbps, 1);
    config_.user_quality_kbps = std::max(config_.user_quality_kbps, 0);
    config_.ideal_demand_kbps = std::max(config_.ideal_demand_kbps, 0);
    config_.fps_needed_kbps = std::max(config_.fps_needed_kbps, 0);
    config_.max_fec_percentage = clamp_percent(config_.max_fec_percentage);
    config_.baseline_fec_percentage = clamp_percent(config_.baseline_fec_percentage, config_.max_fec_percentage);
    if (config_.ceiling_total_bitrate_kbps <= 0) {
      config_.ceiling_total_bitrate_kbps = total_bitrate_for_encoding_bitrate(config_.baseline_bitrate_kbps,
                                                                              config_.baseline_fec_percentage);
    }
    config_.ceiling_total_bitrate_kbps = std::max(config_.ceiling_total_bitrate_kbps, 1);
    const auto configured_ceiling = configured_encoding_ceiling_kbps(config_, config_.baseline_fec_percentage);
    config_.min_bitrate_kbps = std::min(config_.min_bitrate_kbps,
                                        std::max(1, configured_ceiling));
    config_.baseline_bitrate_kbps = std::max(config_.baseline_bitrate_kbps, config_.min_bitrate_kbps);
    const auto startup_encoding_limit = std::max(config_.min_bitrate_kbps, configured_ceiling);
    const auto startup_encoding_floor = std::min(config_.min_bitrate_kbps,
                                                 std::max(1, startup_encoding_limit));
    config_.startup_bitrate_kbps = config_.startup_bitrate_kbps > 0 ?
                                     std::clamp(config_.startup_bitrate_kbps, startup_encoding_floor, startup_encoding_limit) :
                                     startup_encoding_limit;
    config_.baseline_fps = std::clamp(config_.baseline_fps <= 0 ? 60 : config_.baseline_fps, 1, 240);
    const auto default_min_fps = config_.baseline_fps >= 90 ? 60 :
                                 config_.baseline_fps >= 60 ? 45 :
                                 config_.baseline_fps >= 30 ? 18 :
                                 std::max(1, config_.baseline_fps / 2);
    config_.min_fps = std::clamp(config_.min_fps <= 0 ? default_min_fps : config_.min_fps, 1, config_.baseline_fps);
    config_.startup_fps = config_.startup_fps > 0 ?
                            std::clamp(config_.startup_fps, config_.min_fps, config_.baseline_fps) :
                            config_.baseline_fps;
    config_.chroma_sampling_type = config_.chroma_sampling_type >= 0 ? config_.chroma_sampling_type : -1;
    config_.dynamic_range = config_.dynamic_range >= 0 ? config_.dynamic_range : -1;
    current_bitrate_kbps_ = config_.startup_bitrate_kbps;
    current_fec_percentage_ = config_.baseline_fec_percentage;
    current_resolution_scale_percent_ = 100;
    current_chroma_sampling_type_ = config_.chroma_sampling_type;
    current_dynamic_range_ = config_.dynamic_range;
    current_quality_tier_ = 0;
    requested_ceiling_kbps_ = user_quality_budget_kbps(config_);
    effective_ceiling_kbps_ = configured_encoding_ceiling_kbps(config_, current_fec_percentage_);
    sustainable_estimate_kbps_ = config_.startup_bitrate_kbps;
    sustainable_limit_active_ = false;
    pacing_bitrate_kbps_ = clamp_pacing_budget(
      pacing_budget_for_encoding_bitrate(current_bitrate_kbps_, current_fec_percentage_),
      config_.ceiling_total_bitrate_kbps,
      config_.min_bitrate_kbps);
    current_fps_ = config_.startup_fps;
    state_ = state_e::healthy;
    stable_windows_ = 0;
    video_deadline_windows_ = 0;
    fps_adjust_cooldown_windows_ = 0;
    profile_tier_cooldown_windows_ = 0;
    media_recovery_cooldown_windows_ = 0;
    idr_cooldown_windows_ = 0;
    audio_cooldown_windows_ = 0;
    ewma_loss_ = 0.0;
    ewma_unrecoverable_ = 0.0;
    ewma_jitter_ = 0.0;
    ewma_deadline_pressure_ = 0.0;
    ewma_input_pressure_ = 0.0;
    ewma_audio_pressure_ = 0.0;
    ewma_motion_pressure_ = 0.0;
    ewma_delay_pressure_ = 0.0;
    ewma_burst_pressure_ = 0.0;
    configured_ = true;
  }

  action_t
  controller_t::on_feedback(const feedback_t &feedback) {
    if (!configured_) {
      configure({});
    }

    const auto frame_samples = feedback_frame_sample_count(feedback);
    const auto packet_loss = ratio(feedback.missing_packets, feedback.total_packets);
    const auto loss = clamp01(packet_loss);
    const auto unrecoverable = clamp01(ratio(feedback.unrecoverable_frames, frame_samples));
    const auto recovered = clamp01(ratio(feedback.recovered_frames, frame_samples));
    const auto jitter = static_cast<double>(feedback.rtt_variance_ms);
    const auto late = clamp01(ratio(feedback.late_frames, frame_samples));
    const auto decode_queue = static_cast<double>(feedback.decode_queue_depth);
    const auto render_queue = static_cast<double>(feedback.render_queue_depth);
    const auto input_latency_ms = static_cast<double>(
                                    std::max(feedback.input_send_latency_us, feedback.input_ack_latency_us)) /
                                  1000.0;
    const auto audio_pressure = audio_continuity_pressure(feedback);
    const auto audio_pressure_signal = clamp01(audio_pressure * 4.0);
    const bool static_idle_without_video_samples = is_static_idle_without_video_samples(feedback);
    const auto network_rtt_ms = static_idle_without_video_samples ? 0U : feedback.rtt_ms;
    const auto network_jitter_ms = static_idle_without_video_samples ? 0.0 : jitter;
    const auto deadline_jitter_ms = static_idle_without_video_samples ? 0.0 : jitter;
    const auto deadline_pressure = std::max({
      late * 3.0,
      decode_queue / 4.0,
      render_queue / 3.0,
      deadline_jitter_ms / 90.0,
    });
    const auto input_pressure = std::max(static_cast<double>(feedback.input_queue_depth) / 4.0,
                                         input_latency_ms / 80.0);
    const bool input_active = feedback.input_queue_depth > 0 ||
                              feedback.input_send_latency_us > 0 ||
                              feedback.input_ack_latency_us > 0;
    const auto frame_area = feedback.frame_area > 0 ? feedback.frame_area : configured_frame_area(config_);
    const auto dirty_ratio = frame_area > 0 ?
                               std::clamp(static_cast<double>(feedback.dirty_area) /
                                            static_cast<double>(frame_area),
                                          0.0,
                                          1.0) :
                               0.0;
    const auto current_bpp = bits_per_pixel_per_frame(current_bitrate_kbps_,
                                                      std::max(1, current_fps_),
                                                      frame_area);
    const auto target_bpp = readable_bpp_target(current_chroma_sampling_type_);
    const auto bpp_pressure = target_bpp > 0.0 ? clamp01((target_bpp - current_bpp) / target_bpp) : 0.0;
    const bool motion_evidence = feedback.full_frame_dirty || dirty_ratio >= 0.12;
    const auto profile_bpp_pressure = motion_evidence ? bpp_pressure : 0.0;
    const auto bpp_motion_pressure = motion_evidence ?
                                       bpp_pressure * (feedback.full_frame_dirty ? 0.90 :
                                                       dirty_ratio >= 0.45 ? 0.80 :
                                                       0.55) :
                                       0.0;
    const auto recovered_total = recovered + unrecoverable;
    const auto fec_efficiency = recovered_total > 0.0 ? recovered / recovered_total : 0.0;
    const bool observed_packet_loss = loss >= 0.01 ||
                                      unrecoverable > 0.0 ||
                                      recovered >= 0.015 ||
                                      feedback.missing_packets > 0;
    const bool rfi_has_loss_evidence = observed_packet_loss ||
                                       feedback.unrecoverable_frames > 0 ||
                                       feedback.recovered_frames > 0 ||
                                       feedback.missing_packets > 0;
    const auto rfi_pressure = clamp01(std::max(ratio(feedback.rfi_requests,
                                                     frame_samples),
                                               ratio(feedback.waiting_for_rfi_frames,
                                                     frame_samples)));
    const auto network_rfi_pressure = rfi_has_loss_evidence ? rfi_pressure : 0.0;
    pressure_signals_t pressures {
      .random_loss = clamp01(std::max({ loss * 4.0, recovered * 2.0, unrecoverable * 3.0 })),
      .burst_loss = clamp01(std::max({ unrecoverable * 7.0,
                                       ratio(feedback.missing_packets, std::max(feedback.total_packets, 1U)) * 4.0,
                                       network_rfi_pressure })),
      .delay_congestion = clamp01(std::max({ static_cast<double>(network_rtt_ms) / 500.0,
                                             network_jitter_ms / 180.0,
                                             input_latency_ms / 180.0 })),
      .motion = clamp01(std::max({ feedback.full_frame_dirty ? 1.0 : dirty_ratio,
                                   dirty_ratio >= 0.85 ? 0.90 : dirty_ratio * 0.85,
                                   bpp_motion_pressure })),
      .render = clamp01(deadline_pressure / 1.6),
      .audio = audio_pressure_signal,
      .input = clamp01(input_pressure / 1.25),
    };
    const auto emergency_fps_floor = std::clamp(high_refresh_emergency_floor(config_.baseline_fps),
                                                config_.min_fps,
                                                config_.baseline_fps);
    const auto interactive_fps_floor = std::clamp(high_refresh_interactive_floor(config_.baseline_fps),
                                                  emergency_fps_floor,
                                                  config_.baseline_fps);
    const bool motion_active = feedback.full_frame_dirty ||
                               dirty_ratio >= 0.45 ||
                               pressures.motion >= 0.55;
    const auto pressure_fps_floor = (input_active || motion_active) ? interactive_fps_floor : emergency_fps_floor;

    const bool raw_network_clean = loss <= 0.002 &&
                                   unrecoverable == 0.0 &&
                                   recovered <= 0.005 &&
                                   network_jitter_ms <= 18.0;
    const bool raw_video_deadline_clean = late == 0.0 &&
                                          decode_queue == 0.0 &&
                                          render_queue == 0.0 &&
                                          input_pressure <= 0.15;
    const bool raw_deadline_clean = raw_video_deadline_clean &&
                                    audio_pressure == 0.0;
    const bool raw_scene_still = !feedback.full_frame_dirty && dirty_ratio <= 0.05;
    const bool raw_motion_clean = raw_scene_still ||
                                  (pressures.motion <= 0.20 && bpp_pressure <= 0.20);

    ewma_loss_ = ewma(ewma_loss_, loss, raw_network_clean ? 0.72 : 0.45);
    ewma_unrecoverable_ = ewma(ewma_unrecoverable_, unrecoverable, raw_network_clean ? 0.72 : 0.55);
    ewma_jitter_ = ewma(ewma_jitter_, network_jitter_ms, raw_network_clean ? 0.65 : 0.35);
    ewma_deadline_pressure_ = ewma(ewma_deadline_pressure_, deadline_pressure, raw_deadline_clean ? 0.68 : 0.45);
    ewma_input_pressure_ = ewma(ewma_input_pressure_, input_pressure, raw_deadline_clean ? 0.68 : 0.5);
    ewma_motion_pressure_ = ewma(ewma_motion_pressure_, pressures.motion, raw_motion_clean ? 0.62 : 0.50);
    ewma_delay_pressure_ = ewma(ewma_delay_pressure_, pressures.delay_congestion, raw_network_clean ? 0.65 : 0.40);
    ewma_burst_pressure_ = ewma(ewma_burst_pressure_, pressures.burst_loss, raw_network_clean ? 0.65 : 0.52);
    const bool raw_audio_clean = feedback.audio_underruns == 0 &&
                                 feedback.audio_concealed_ms == 0 &&
                                 feedback.late_audio_drops == 0 &&
                                 feedback.audio_plc_ms == 0 &&
                                 feedback.audio_fade_ms == 0 &&
                                 feedback.audio_buffer_depth_ms <= 120;
    ewma_audio_pressure_ = ewma(ewma_audio_pressure_, audio_pressure_signal, raw_audio_clean ? 0.72 : 0.5);
    if (!static_idle_without_video_samples) {
      pressures.motion = std::max(pressures.motion, ewma_motion_pressure_);
      pressures.delay_congestion = std::max(pressures.delay_congestion, ewma_delay_pressure_);
      pressures.burst_loss = std::max(pressures.burst_loss, ewma_burst_pressure_);
    }
    pressures.audio = clamp01(std::max(pressures.audio, ewma_audio_pressure_));

    const auto previous_bitrate = current_bitrate_kbps_;
    const auto previous_fec = current_fec_percentage_;
    const auto previous_fps = current_fps_;
    const auto previous_state = state_;
    const auto previous_resolution_scale = current_resolution_scale_percent_;
    const auto previous_chroma_sampling_type = current_chroma_sampling_type_;
    const auto previous_dynamic_range = current_dynamic_range_;
    const auto previous_quality_tier = current_quality_tier_;
    reason_e reason = reason_e::healthy;
    const bool raw_video_deadline_miss = deadline_pressure >= 0.95;
    const bool hard_video_deadline_miss = ewma_deadline_pressure_ >= 1.15;
    const bool input_constrained = input_pressure >= 1.1 || ewma_input_pressure_ >= 1.25;
    const bool rfi_storm = rfi_has_loss_evidence &&
                           (feedback.rfi_requests >= 8 ||
                            feedback.waiting_for_rfi_frames >= std::max(8U, feedback.frames_seen / 3U) ||
                            network_rfi_pressure >= 0.95);
    const bool motion_constrained = pressures.motion >= 0.55 &&
                                    bpp_pressure >= 0.25 &&
                                    (input_active || feedback.full_frame_dirty || dirty_ratio >= 0.45);
    const bool raw_network_crisis = unrecoverable >= 0.10 ||
                                    (unrecoverable >= 0.02 && loss >= 0.12) ||
                                    network_jitter_ms >= 130.0 ||
                                    rfi_storm;
    const bool raw_network_constrained = unrecoverable >= 0.015 ||
                                         loss >= 0.04 ||
                                         recovered >= 0.05 ||
                                         network_jitter_ms >= 45.0;
    const bool network_crisis = raw_network_crisis ||
                                (!raw_network_clean &&
                                 (ewma_unrecoverable_ >= 0.12 ||
                                  (ewma_unrecoverable_ >= 0.02 && ewma_loss_ >= 0.16) ||
                                  ewma_jitter_ >= 110.0));
    const bool network_constrained = raw_network_constrained ||
                                     (!raw_network_clean &&
                                      (ewma_unrecoverable_ >= 0.015 ||
                                       ewma_loss_ >= 0.035 ||
                                       ewma_jitter_ >= 40.0));
    const bool deadline_crisis = ewma_deadline_pressure_ >= 1.7;
    const bool video_deadline_constrained = raw_video_deadline_miss || hard_video_deadline_miss || deadline_crisis;
    const bool delay_only_congestion = !observed_packet_loss &&
                                       (pressures.delay_congestion >= 0.80 ||
                                        network_jitter_ms >= 90.0 ||
                                        network_rtt_ms >= 250 ||
                                        video_deadline_constrained);
    const bool fec_recoverable_loss = observed_packet_loss &&
                                      feedback.unrecoverable_frames == 0 &&
                                      unrecoverable < 0.005 &&
                                      !rfi_storm;
    const bool random_loss_recovered_by_fec = recovered >= 0.05 &&
                                              unrecoverable < 0.005 &&
                                              feedback.unrecoverable_frames == 0;
    const bool severe_random_loss = observed_packet_loss &&
                                    !fec_recoverable_loss &&
                                    (unrecoverable >= 0.05 ||
                                     (loss >= 0.12 && !random_loss_recovered_by_fec) ||
                                     rfi_storm);
    const bool moderate_random_loss = observed_packet_loss &&
                                      (unrecoverable >= 0.005 ||
                                       recovered >= 0.05 ||
                                       loss >= 0.04);
    const bool preserve_readable_interactive_floor = (input_active || motion_active) &&
                                                     feedback.unrecoverable_frames == 0 &&
                                                     unrecoverable < 0.005 &&
                                                     !rfi_storm &&
                                                     network_rtt_ms < 900 &&
                                                     network_jitter_ms < 250.0;
    const bool audio_constrained = feedback.audio_underruns >= 2 ||
                                   feedback.late_audio_drops > 0 ||
                                   feedback.audio_concealed_ms >= 20 ||
                                   feedback.audio_plc_ms >= 20 ||
                                   feedback.audio_fade_ms >= 10 ||
                                   ewma_audio_pressure_ >= 0.30;
    const bool audio_crisis = feedback.audio_underruns >= 8 ||
                              feedback.late_audio_drops >= 6 ||
                              feedback.audio_concealed_ms >= 150 ||
                              feedback.audio_plc_ms >= 80 ||
                              ewma_audio_pressure_ >= 0.55;
    const bool fps_budget_overshoot_allowed =
      config_.fps_needed_kbps > user_quality_budget_kbps(config_) &&
      raw_network_clean &&
      raw_video_deadline_clean &&
      !audio_constrained &&
      !input_constrained;
    const bool media_stability_crisis =
      (audio_crisis && observed_packet_loss) ||
      (rfi_storm && (unrecoverable >= 0.02 || ewma_burst_pressure_ >= 0.70));
    const auto observed_video_kbps = observed_video_bitrate_kbps(feedback);
    const bool has_video_delivery_samples = feedback.frames_seen > 0 ||
                                            feedback.displayed_frames > 0 ||
                                            feedback.total_packets > 0 ||
                                            feedback.video_bytes > 0;
    const auto pre_action_encoding_ceiling = configured_encoding_ceiling_kbps(config_, current_fec_percentage_);

    if (idr_cooldown_windows_ > 0) {
      --idr_cooldown_windows_;
    }
    if (audio_cooldown_windows_ > 0) {
      --audio_cooldown_windows_;
    }
    if (fps_adjust_cooldown_windows_ > 0) {
      --fps_adjust_cooldown_windows_;
    }
    if (profile_tier_cooldown_windows_ > 0) {
      --profile_tier_cooldown_windows_;
    }
    if (media_recovery_cooldown_windows_ > 0) {
      --media_recovery_cooldown_windows_;
    }

    video_deadline_windows_ = video_deadline_constrained ?
                                std::min(video_deadline_windows_ + 1, 16) :
                                0;
    auto reduce_fps_for_pressure = [&](double scale,
                                       int cooldown_windows,
                                       int required_pressure_windows) {
      if (fps_adjust_cooldown_windows_ > 0 ||
          video_deadline_windows_ < required_pressure_windows) {
        return;
      }

      current_fps_ = clamp_fps(static_cast<int>(std::lround(current_fps_ * scale)),
                               pressure_fps_floor,
                               config_.baseline_fps);
      fps_adjust_cooldown_windows_ = cooldown_windows;
    };
    auto bleed_fec_for_delay_only = [&]() {
      if (current_fec_percentage_ > config_.baseline_fec_percentage) {
        current_fec_percentage_ = approach_int(current_fec_percentage_,
                                               config_.baseline_fec_percentage,
                                               0.40,
                                               3,
                                               16);
      }
    };
    auto bleed_ineffective_fec = [&]() {
      if (current_fec_percentage_ > config_.baseline_fec_percentage) {
        current_fec_percentage_ = approach_int(current_fec_percentage_,
                                               config_.baseline_fec_percentage,
                                               0.50,
                                               4,
                                               20);
      }
    };

    const bool audio_only_pressure = audio_constrained &&
                                     !network_crisis &&
                                     !network_constrained &&
                                     !video_deadline_constrained &&
                                     !input_constrained;
    const auto audio_only_floor_basis = has_video_delivery_samples ?
                                          pre_action_encoding_ceiling :
                                          std::min(current_bitrate_kbps_, config_.startup_bitrate_kbps);
    const auto audio_only_floor = std::max(
      config_.min_bitrate_kbps,
      static_cast<int>(std::lround(static_cast<double>(audio_only_floor_basis) *
                                   (audio_crisis ? 0.92 : 0.96))));

    if (network_crisis) {
      state_ = state_e::crisis;
      reason = delay_only_congestion ? reason_e::delay_congestion :
               motion_constrained ? reason_e::motion_pressure :
               reason_e::random_loss;
      stable_windows_ = 0;
      if (rfi_storm || feedback.waiting_for_rfi_frames > 0 || unrecoverable >= 0.02) {
        media_recovery_cooldown_windows_ = std::max(media_recovery_cooldown_windows_, 8);
      }
      if (video_deadline_constrained) {
        reduce_fps_for_pressure(0.88, 0, 1);
      }
      if (media_stability_crisis) {
        reduce_fps_for_pressure(0.86, 0, 1);
        current_bitrate_kbps_ = std::max(config_.min_bitrate_kbps,
                                         static_cast<int>(std::lround(current_bitrate_kbps_ * 0.72)));
      }
      if (delay_only_congestion) {
        bleed_fec_for_delay_only();
        const auto scale = preserve_readable_interactive_floor ? 0.90 : 0.76;
        current_bitrate_kbps_ = std::max(config_.min_bitrate_kbps, static_cast<int>(std::lround(current_bitrate_kbps_ * scale)));
        if (config_.user_quality_kbps > 0 && current_bitrate_kbps_ > user_quality_budget_kbps(config_)) {
          current_bitrate_kbps_ = std::min(
            current_bitrate_kbps_,
            std::max(config_.min_bitrate_kbps,
                     static_cast<int>(std::lround(static_cast<double>(user_quality_budget_kbps(config_)) * 1.30))));
        }
      }
      else if (unrecoverable >= 0.05 || fec_efficiency < 0.55) {
        if (fec_efficiency < 0.20) {
          bleed_ineffective_fec();
        }
        current_bitrate_kbps_ = std::max(config_.min_bitrate_kbps, static_cast<int>(std::lround(current_bitrate_kbps_ * 0.85)));
      }
      if (!delay_only_congestion && severe_random_loss) {
        const auto raw_target_fec = linear_fec_target_for_random_loss(config_.baseline_fec_percentage,
                                                                      config_.max_fec_percentage,
                                                                      loss,
                                                                      recovered,
                                                                      unrecoverable);
        const auto target_fec = cap_fec_for_observed_efficiency(raw_target_fec,
                                                                config_.baseline_fec_percentage,
                                                                fec_efficiency,
                                                                unrecoverable,
                                                                recovered);
        const auto stability_target_fec = media_stability_crisis ?
                                            std::min(target_fec, std::max(config_.baseline_fec_percentage + 50, 60)) :
                                            target_fec;
        if (media_stability_crisis &&
            current_fec_percentage_ > stability_target_fec) {
          current_fec_percentage_ = approach_int(current_fec_percentage_,
                                                 stability_target_fec,
                                                 0.35,
                                                 5,
                                                 20);
        }
        else if (unrecoverable < 0.02 &&
            fec_efficiency >= 0.90 &&
            current_fec_percentage_ > stability_target_fec) {
          current_fec_percentage_ = approach_int(current_fec_percentage_,
                                                 stability_target_fec,
                                                 0.18,
                                                 1,
                                                 8);
        }
        else {
          current_fec_percentage_ = std::max(current_fec_percentage_, stability_target_fec);
        }
      }
    }
    else if (network_constrained) {
      state_ = state_e::constrained;
      reason = delay_only_congestion ? reason_e::delay_congestion :
               motion_constrained ? reason_e::motion_pressure :
               reason_e::random_loss;
      stable_windows_ = 0;
      if (video_deadline_constrained) {
        reduce_fps_for_pressure(0.92, delay_only_congestion ? 2 : 1, 1);
      }
      if (delay_only_congestion) {
        bleed_fec_for_delay_only();
        const auto scale = preserve_readable_interactive_floor ? 0.96 : 0.88;
        current_bitrate_kbps_ = std::max(config_.min_bitrate_kbps, static_cast<int>(std::lround(current_bitrate_kbps_ * scale)));
        if (config_.user_quality_kbps > 0 && current_bitrate_kbps_ > user_quality_budget_kbps(config_)) {
          current_bitrate_kbps_ = std::min(
            current_bitrate_kbps_,
            std::max(config_.min_bitrate_kbps,
                     static_cast<int>(std::lround(static_cast<double>(user_quality_budget_kbps(config_)) * 1.30))));
        }
      }
      else if (fec_recoverable_loss && fec_efficiency >= 0.90) {
        current_bitrate_kbps_ = std::min(pre_action_encoding_ceiling,
                                         current_bitrate_kbps_ + gentle_probe_step(current_bitrate_kbps_,
                                                                                   pre_action_encoding_ceiling));
      }
      else if (!fec_recoverable_loss) {
        if (fec_efficiency < 0.20) {
          bleed_ineffective_fec();
        }
        current_bitrate_kbps_ = std::max(config_.min_bitrate_kbps, static_cast<int>(std::lround(current_bitrate_kbps_ * 0.94)));
      }
      if (!delay_only_congestion && moderate_random_loss) {
        const auto raw_target_fec = linear_fec_target_for_random_loss(config_.baseline_fec_percentage,
                                                                      config_.max_fec_percentage,
                                                                      loss,
                                                                      recovered,
                                                                      unrecoverable);
        const auto target_fec = cap_fec_for_observed_efficiency(raw_target_fec,
                                                                config_.baseline_fec_percentage,
                                                                fec_efficiency,
                                                                unrecoverable,
                                                                recovered);
        const auto stability_target_fec = media_stability_crisis ?
                                            std::min(target_fec, std::max(config_.baseline_fec_percentage + 50, 60)) :
                                            target_fec;
        if (media_stability_crisis &&
            current_fec_percentage_ > stability_target_fec) {
          current_fec_percentage_ = approach_int(current_fec_percentage_,
                                                 stability_target_fec,
                                                 0.35,
                                                 5,
                                                 20);
        }
        else if (random_loss_recovered_by_fec &&
            fec_efficiency >= 0.92 &&
            current_fec_percentage_ > stability_target_fec) {
          current_fec_percentage_ = approach_int(current_fec_percentage_,
                                                 stability_target_fec,
                                                 0.25,
                                                 1,
                                                 10);
        }
        else {
          current_fec_percentage_ = std::max(current_fec_percentage_, stability_target_fec);
        }
      }
    }
    else if (motion_constrained) {
      state_ = state_e::constrained;
      reason = reason_e::motion_pressure;
      stable_windows_ = 0;
      if (fps_budget_overshoot_allowed) {
        current_bitrate_kbps_ = std::min(pre_action_encoding_ceiling,
                                         current_bitrate_kbps_ + gentle_probe_step(current_bitrate_kbps_,
                                                                                   pre_action_encoding_ceiling));
      }
    }
    else if (audio_constrained) {
      state_ = state_e::constrained;
      reason = reason_e::audio_pressure;
      if (audio_only_pressure && raw_network_clean && raw_video_deadline_clean && raw_motion_clean) {
        stable_windows_ = std::min(stable_windows_ + 1, 60);
      }
      else {
        stable_windows_ = 0;
      }
      if (audio_only_pressure && raw_network_clean && current_fec_percentage_ > config_.baseline_fec_percentage) {
        current_fec_percentage_ = approach_int(current_fec_percentage_,
                                               config_.baseline_fec_percentage,
                                               0.55,
                                               2,
                                               18);
      }
      if (audio_cooldown_windows_ == 0) {
        const auto audio_signal = std::max(audio_pressure, ewma_audio_pressure_);
        current_bitrate_kbps_ = std::max(config_.min_bitrate_kbps,
                                         static_cast<int>(std::lround(current_bitrate_kbps_ *
                                                                      linear_audio_bitrate_scale(audio_signal,
                                                                                                 audio_crisis))));
        if (audio_only_pressure) {
          current_bitrate_kbps_ = std::max(current_bitrate_kbps_, audio_only_floor);
        }
        audio_cooldown_windows_ = 2;
      }
      if (audio_only_pressure) {
        current_fps_ = approach_int(current_fps_,
                                    config_.baseline_fps,
                                    0.22,
                                    4,
                                    std::max(8, config_.baseline_fps / 10));
        const auto audio_only_recovery_ceiling = std::min(
          configured_encoding_ceiling_kbps(config_, current_fec_percentage_),
          effective_ceiling_kbps_);
        const auto audio_only_target = std::min(
          audio_only_recovery_ceiling,
          std::max(audio_only_floor,
                   current_bitrate_kbps_ + gentle_probe_step(current_bitrate_kbps_,
                                                             audio_only_recovery_ceiling)));
        if (current_bitrate_kbps_ < audio_only_floor &&
            current_bitrate_kbps_ < audio_only_target) {
          current_bitrate_kbps_ = approach_int(current_bitrate_kbps_,
                                               audio_only_target,
                                               0.65,
                                               1000,
                                               7000);
        }
      }
    }
    else if (video_deadline_constrained) {
      state_ = state_e::constrained;
      reason = reason_e::render_deadline;
      stable_windows_ = 0;
      reduce_fps_for_pressure(hard_video_deadline_miss ? 0.93 : 0.96,
                              hard_video_deadline_miss ? 2 : 3,
                              hard_video_deadline_miss ? 1 : 2);
    }
    else if (input_constrained) {
      state_ = state_e::constrained;
      reason = reason_e::input_pressure;
      stable_windows_ = 0;
    }
    else {
      stable_windows_++;
      if (current_bitrate_kbps_ < configured_encoding_ceiling_kbps(config_, current_fec_percentage_) ||
          current_fec_percentage_ > config_.baseline_fec_percentage ||
          current_fps_ < config_.baseline_fps ||
          current_resolution_scale_percent_ < 100 ||
          (config_.chroma_sampling_type >= 0 && current_chroma_sampling_type_ != config_.chroma_sampling_type) ||
          (config_.dynamic_range >= 0 && current_dynamic_range_ != config_.dynamic_range)) {
        state_ = state_e::recovering;
        reason = reason_e::recovering;
        if (stable_windows_ >= 3 && current_fec_percentage_ > config_.baseline_fec_percentage) {
          current_fec_percentage_ = std::max(config_.baseline_fec_percentage,
                                             current_fec_percentage_ - (stable_windows_ >= 7 ? 2 : 1));
        }
        const auto bitrate_recovery_ceiling = std::min(
          configured_encoding_ceiling_kbps(config_, current_fec_percentage_),
          effective_ceiling_kbps_);
        current_fps_ = approach_int(current_fps_,
                                    config_.baseline_fps,
                                    0.16,
                                    1,
                                    std::max(4, config_.baseline_fps / 12));
        current_bitrate_kbps_ = std::min(bitrate_recovery_ceiling,
                                         current_bitrate_kbps_ + gentle_probe_step(current_bitrate_kbps_,
                                                                                   bitrate_recovery_ceiling));
      }
      else {
        state_ = state_e::healthy;
        reason = reason_e::healthy;
      }
    }

    const auto configured_encoding_ceiling = configured_encoding_ceiling_kbps(config_, current_fec_percentage_);
    requested_ceiling_kbps_ = user_quality_budget_kbps(config_);
    auto readable_floor_kbps = preserve_readable_interactive_floor ?
                                 readable_interactive_floor_kbps(config_, configured_encoding_ceiling) :
                                 std::min(config_.min_bitrate_kbps, configured_encoding_ceiling);
    const bool weak_route_recovery_guard =
      sustainable_limit_active_ ||
      network_crisis ||
      network_constrained ||
      rfi_storm ||
      audio_constrained ||
      ewma_unrecoverable_ >= 0.005 ||
      ewma_burst_pressure_ >= 0.30 ||
      ewma_delay_pressure_ >= 0.35 ||
      current_fec_percentage_ >= std::min(config_.max_fec_percentage,
                                          config_.baseline_fec_percentage + 35);
    if (preserve_readable_interactive_floor &&
        weak_route_recovery_guard &&
        sustainable_estimate_kbps_ > 0) {
      const auto sustainable_floor_cap = std::clamp(
        sustainable_estimate_kbps_ + std::max(3000,
                                              static_cast<int>(std::lround(
                                                static_cast<double>(sustainable_estimate_kbps_) * 0.20))),
        std::min(config_.min_bitrate_kbps, configured_encoding_ceiling),
        std::max(config_.min_bitrate_kbps, configured_encoding_ceiling));
      readable_floor_kbps = std::min(readable_floor_kbps, sustainable_floor_cap);
    }

    const bool should_constrain_sustainable_estimate =
      delay_only_congestion ||
      unrecoverable >= 0.005 ||
      ewma_unrecoverable_ >= 0.005 ||
      (loss >= 0.12 && fec_efficiency < 0.75);

    if ((network_crisis || network_constrained) &&
        should_constrain_sustainable_estimate &&
        observed_video_kbps > 0) {
      sustainable_limit_active_ = true;
      auto congestion_sample = std::clamp(
        static_cast<int>(std::lround(static_cast<double>(observed_video_kbps) * 1.20)),
        config_.min_bitrate_kbps,
        std::max(config_.min_bitrate_kbps, configured_encoding_ceiling));
      if (preserve_readable_interactive_floor) {
        congestion_sample = std::max(congestion_sample, readable_floor_kbps);
      }
      sustainable_estimate_kbps_ = sustainable_estimate_kbps_ > 0 ?
                                     approach_int(sustainable_estimate_kbps_,
                                                  congestion_sample,
                                                  congestion_sample < sustainable_estimate_kbps_ ? 0.50 : 0.20,
                                                  500,
                                                  20000) :
                                     congestion_sample;
    }
    else if (raw_network_clean &&
             (raw_deadline_clean || (audio_only_pressure && raw_video_deadline_clean)) &&
             observed_video_kbps > 0) {
      const auto observed_headroom_kbps =
        static_cast<int>(std::lround(static_cast<double>(observed_video_kbps) * 1.30));
      const auto clean_sample = std::clamp(
        sustainable_limit_active_ ? observed_headroom_kbps :
                                    std::max(current_bitrate_kbps_, observed_headroom_kbps),
        config_.min_bitrate_kbps,
        std::max(config_.min_bitrate_kbps, configured_encoding_ceiling));
      sustainable_estimate_kbps_ = sustainable_estimate_kbps_ > 0 ?
                                     approach_int(sustainable_estimate_kbps_,
                                                  clean_sample,
                                                  clean_sample > sustainable_estimate_kbps_ ? 0.18 : 0.08,
                                                  250,
                                                  8000) :
                                     clean_sample;
    }
    else if (sustainable_estimate_kbps_ <= 0) {
      sustainable_estimate_kbps_ = std::max(current_bitrate_kbps_, config_.min_bitrate_kbps);
    }

    if (audio_only_pressure) {
      sustainable_limit_active_ = false;
      sustainable_estimate_kbps_ = std::max(sustainable_estimate_kbps_, current_bitrate_kbps_);
      effective_ceiling_kbps_ = configured_encoding_ceiling;
    }

    if (sustainable_limit_active_) {
      const bool tight_sustainable_budget =
        (network_crisis || network_constrained) &&
        (media_stability_crisis ||
         rfi_storm ||
         audio_crisis ||
         ewma_burst_pressure_ >= 0.70 ||
         current_fec_percentage_ >= 80);
      const auto headroom = tight_sustainable_budget ?
                              std::max(1500,
                                       static_cast<int>(std::lround(static_cast<double>(sustainable_estimate_kbps_) * 0.15))) :
                              std::max(6000,
                                       static_cast<int>(std::lround(static_cast<double>(sustainable_estimate_kbps_) * 0.25)) +
                                         stable_windows_ * 250);
      effective_ceiling_kbps_ = std::clamp(sustainable_estimate_kbps_ + headroom,
                                           std::max(config_.min_bitrate_kbps, current_bitrate_kbps_),
                                           std::max(config_.min_bitrate_kbps, configured_encoding_ceiling));
      if (preserve_readable_interactive_floor) {
        effective_ceiling_kbps_ = std::max(effective_ceiling_kbps_, readable_floor_kbps);
      }
      if (stable_windows_ >= 48 &&
          current_bitrate_kbps_ >= effective_ceiling_kbps_ - 1000 &&
          effective_ceiling_kbps_ < configured_encoding_ceiling) {
        sustainable_limit_active_ = false;
      }
    }

    if (!sustainable_limit_active_) {
      effective_ceiling_kbps_ = configured_encoding_ceiling;
    }

    const bool profile_recovery_clean = raw_network_clean &&
                                        raw_video_deadline_clean &&
                                        raw_motion_clean &&
                                        (!audio_constrained || audio_only_pressure) &&
                                        stable_windows_ >= 3;
    const auto target_scale = profile_recovery_clean ?
                                100 :
                                scale_target_for_pressure(pressures.motion,
                                                          pressures.burst_loss,
                                                          profile_bpp_pressure,
                                                          input_active);
    current_resolution_scale_percent_ = approach_int(current_resolution_scale_percent_,
                                                     target_scale,
                                                     target_scale < current_resolution_scale_percent_ ? 0.65 : 0.22,
                                                     5,
                                                     target_scale < current_resolution_scale_percent_ ? 15 : 10);

    if (config_.chroma_sampling_type >= 0) {
      const auto target_chroma = (!profile_recovery_clean &&
                                  config_.chroma_sampling_type == 1 &&
                                  (current_resolution_scale_percent_ < 100 ||
                                   pressures.motion >= 0.62 ||
                                   pressures.burst_loss >= 0.60)) ?
                                   0 :
                                   config_.chroma_sampling_type;
      current_chroma_sampling_type_ = target_chroma;
    }

    if (config_.dynamic_range >= 0) {
      const auto high_profile_pressure = std::max({ pressures.burst_loss,
                                                    pressures.delay_congestion,
                                                    pressures.motion,
                                                    pressures.render });
      const auto target_dynamic_range = (!profile_recovery_clean &&
                                         config_.dynamic_range > 0 &&
                                         high_profile_pressure >= 0.82) ?
                                          0 :
                                          config_.dynamic_range;
      current_dynamic_range_ = target_dynamic_range;
    }
    current_quality_tier_ = std::max(tier_for_scale(current_resolution_scale_percent_),
                                     current_chroma_sampling_type_ == 0 && config_.chroma_sampling_type == 1 ? 1 : 0);

    const bool profile_tier_active =
      current_quality_tier_ > 0 ||
      current_resolution_scale_percent_ < 100 ||
      (config_.chroma_sampling_type >= 0 && current_chroma_sampling_type_ != config_.chroma_sampling_type) ||
      (config_.dynamic_range >= 0 && current_dynamic_range_ != config_.dynamic_range);
    if (profile_tier_active && !config_.runtime_profile_tier_supported && !fps_budget_overshoot_allowed) {
      const auto fallback_fps = fps_target_for_profile_fallback(config_.baseline_fps,
                                                                current_fps_,
                                                                profile_bpp_pressure,
                                                                input_active || motion_active);
      if (fallback_fps < current_fps_ &&
          fps_adjust_cooldown_windows_ == 0) {
        current_fps_ = clamp_fps(fallback_fps,
                                 pressure_fps_floor,
                                 config_.baseline_fps);
        fps_adjust_cooldown_windows_ = 2;
      }
    }

    current_fps_ = clamp_fps(current_fps_,
                             state_ == state_e::healthy ? config_.min_fps : pressure_fps_floor,
                             config_.baseline_fps);
    current_bitrate_kbps_ = std::min(
      current_bitrate_kbps_,
      std::min(effective_ceiling_kbps_,
               configured_encoding_ceiling_kbps(config_, current_fec_percentage_)));
    current_bitrate_kbps_ = std::max(current_bitrate_kbps_,
                                     std::min(readable_floor_kbps, effective_ceiling_kbps_));
    const bool recent_media_recovery =
      media_recovery_cooldown_windows_ > 0 ||
      previous_state == state_e::crisis ||
      rfi_storm ||
      feedback.waiting_for_rfi_frames > 0;
    const bool severe_media_stall =
      network_crisis &&
      (rfi_storm ||
       unrecoverable >= 0.10 ||
       feedback.waiting_for_rfi_frames >= std::max(8U, feedback.frames_seen / 3U) ||
       network_jitter_ms >= 220.0);
    const bool emergency_return_to_user_budget =
      delay_only_congestion &&
      config_.user_quality_kbps > 0 &&
      previous_bitrate > std::max(config_.min_bitrate_kbps,
                                  static_cast<int>(std::lround(static_cast<double>(user_quality_budget_kbps(config_)) * 1.30))) &&
      current_bitrate_kbps_ <= std::max(config_.min_bitrate_kbps,
                                        static_cast<int>(std::lround(static_cast<double>(user_quality_budget_kbps(config_)) * 1.30)));

    if (previous_bitrate > 0) {
      const auto max_bitrate_down = emergency_return_to_user_budget ?
                                      previous_bitrate :
                                      proportional_step(previous_bitrate,
                                                        severe_media_stall ? 0.28 :
                                                        network_crisis ? 0.18 :
                                                        network_constrained ? 0.12 :
                                                        audio_constrained ? 0.06 :
                                                        0.08,
                                                        500,
                                                        severe_media_stall ? 22000 :
                                                        network_crisis ? 14000 :
                                                        network_constrained ? 9000 :
                                                        6000);
      const bool clean_audio_only_visual_recovery =
        audio_only_pressure &&
        raw_network_clean &&
        raw_video_deadline_clean &&
        raw_motion_clean;
      const auto max_bitrate_up = proportional_step(previous_bitrate,
                                                    clean_audio_only_visual_recovery ? 0.35 :
                                                    recent_media_recovery ? 0.18 :
                                                    0.20,
                                                    1200,
                                                    clean_audio_only_visual_recovery ? 7000 :
                                                    recent_media_recovery ? 5000 :
                                                    8000);
      current_bitrate_kbps_ = clamp_delta_int(previous_bitrate,
                                             current_bitrate_kbps_,
                                             max_bitrate_down,
                                             max_bitrate_up);
      current_bitrate_kbps_ = std::min(current_bitrate_kbps_,
                                       std::min(effective_ceiling_kbps_,
                                                configured_encoding_ceiling_kbps(config_, current_fec_percentage_)));
    }

    if (previous_fec >= 0) {
      const auto max_fec_up = state_ == state_e::crisis ? 20 :
                              recent_media_recovery ? 8 :
                              12;
      const auto max_fec_down = delay_only_congestion ? 16 :
                                recent_media_recovery ? 8 :
                                10;
      current_fec_percentage_ = clamp_delta_int(previous_fec,
                                                current_fec_percentage_,
                                                max_fec_down,
                                                max_fec_up);
      current_fec_percentage_ = clamp_percent(current_fec_percentage_, config_.max_fec_percentage);
    }

    if (previous_fps > 0) {
      const auto max_fps_down = severe_media_stall ? 12 : 8;
      const auto max_fps_up = recent_media_recovery ?
                                8 :
                                std::max(4, config_.baseline_fps / 12);
      current_fps_ = clamp_delta_int(previous_fps,
                                     current_fps_,
                                     max_fps_down,
                                     max_fps_up);
      current_fps_ = clamp_fps(current_fps_,
                               state_ == state_e::healthy ? config_.min_fps : pressure_fps_floor,
                               config_.baseline_fps);
    }

    current_bitrate_kbps_ = std::min(
      current_bitrate_kbps_,
      std::min(effective_ceiling_kbps_,
               configured_encoding_ceiling_kbps(config_, current_fec_percentage_)));
    pacing_bitrate_kbps_ = clamp_pacing_budget(
      pacing_budget_for_encoding_bitrate(current_bitrate_kbps_, current_fec_percentage_),
      config_.ceiling_total_bitrate_kbps,
      config_.min_bitrate_kbps);
    const auto total_budget_kbps = total_bitrate_for_encoding_bitrate(current_bitrate_kbps_, current_fec_percentage_);
    const auto fec_budget_kbps = std::max(0, total_budget_kbps - current_bitrate_kbps_);

    const bool request_idr = network_crisis &&
                             !delay_only_congestion &&
                             severe_random_loss &&
                             state_ == state_e::crisis &&
                             previous_state != state_e::crisis &&
                             idr_cooldown_windows_ == 0;
    if (request_idr) {
      idr_cooldown_windows_ = 8;
    }

    return {
      .changed = previous_bitrate != current_bitrate_kbps_ ||
                 previous_fec != current_fec_percentage_ ||
                 previous_fps != current_fps_ ||
                 previous_state != state_ ||
                 previous_resolution_scale != current_resolution_scale_percent_ ||
                 previous_chroma_sampling_type != current_chroma_sampling_type_ ||
                 previous_dynamic_range != current_dynamic_range_ ||
                 previous_quality_tier != current_quality_tier_,
      .state = state_,
      .reason = reason,
      .target_bitrate_kbps = current_bitrate_kbps_,
      .fec_percentage = current_fec_percentage_,
      .pacing_bitrate_kbps = pacing_bitrate_kbps_,
      .target_fps = current_fps_,
      .requested_ceiling_kbps = requested_ceiling_kbps_,
      .effective_ceiling_kbps = effective_ceiling_kbps_,
      .sustainable_estimate_kbps = sustainable_estimate_kbps_,
      .encoding_budget_kbps = current_bitrate_kbps_,
      .fec_budget_kbps = fec_budget_kbps,
      .packet_loss = loss,
      .recovered_loss = recovered,
      .unrecoverable_loss = unrecoverable,
      .fec_efficiency = fec_efficiency,
      .pressures = pressures,
      .resolution_scale_percent = current_resolution_scale_percent_,
      .chroma_sampling_type = current_chroma_sampling_type_,
      .dynamic_range = current_dynamic_range_,
      .quality_tier = current_quality_tier_,
      .profile_tier_changed = profile_tier_active,
      .profile_tier_deferred = profile_tier_active && !config_.runtime_profile_tier_supported,
      .profile_tier_supported = config_.runtime_profile_tier_supported,
      .rfi_limited = rfi_storm && !request_idr,
      .request_idr = request_idr,
    };
  }

  int
  controller_t::current_bitrate_kbps() const {
    return current_bitrate_kbps_;
  }

  int
  controller_t::current_fec_percentage() const {
    return current_fec_percentage_;
  }

  int
  controller_t::pacing_bitrate_kbps() const {
    return pacing_bitrate_kbps_;
  }

  int
  controller_t::current_fps() const {
    return current_fps_;
  }

  int
  controller_t::requested_ceiling_kbps() const {
    return requested_ceiling_kbps_;
  }

  int
  controller_t::effective_ceiling_kbps() const {
    return effective_ceiling_kbps_;
  }

  int
  controller_t::sustainable_estimate_kbps() const {
    return sustainable_estimate_kbps_;
  }

  state_e
  controller_t::state() const {
    return state_;
  }

  const char *
  reason_name(reason_e reason) {
    switch (reason) {
      case reason_e::startup:
        return "startup";
      case reason_e::healthy:
        return "healthy";
      case reason_e::recovering:
        return "recovering";
      case reason_e::random_loss:
        return "random-loss";
      case reason_e::delay_congestion:
        return "delay-only-congestion";
      case reason_e::render_deadline:
        return "render-deadline";
      case reason_e::input_pressure:
        return "input-pressure";
      case reason_e::audio_pressure:
        return "audio-pressure";
      case reason_e::motion_pressure:
        return "motion-pressure";
    }

    return "unknown";
  }
}  // namespace weak_net
