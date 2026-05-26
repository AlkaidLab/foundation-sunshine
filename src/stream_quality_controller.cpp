#include "stream_quality_controller.h"

#include "logging.h"

#include <algorithm>
#include <utility>

namespace stream_quality {
  namespace {
    static void log_module_activation(const AlkSessionComponentManifest &manifest,
                                      const config_t &config) {
#ifndef SUNSHINE_TESTS
      BOOST_LOG(info)
        << "Alkaid runtime marker: using Alkaid SDK stream-quality module "
        << "module=" << manifest.component_id << " "
        << "contractVersion=" << ALK_STREAM_QUALITY_CONTROL_VERSION
        << " baseline=" << config.baseline_bitrate_kbps << "Kbps/"
        << config.baseline_fps << "fps/"
        << config.baseline_fec_percentage << "% "
        << "min=" << config.min_bitrate_kbps << "Kbps/"
        << config.min_fps << "fps "
        << "maxFec=" << config.max_fec_percentage << "% "
        << "adapter=Sunshine-stream-quality coreSession=0";
#else
      (void) manifest;
      (void) config;
#endif
    }

    static state_e from_alk_state(uint32_t value) {
      switch (value) {
        case ALK_STREAM_QUALITY_STATE_CONSTRAINED: return state_e::constrained;
        case ALK_STREAM_QUALITY_STATE_CRISIS: return state_e::crisis;
        case ALK_STREAM_QUALITY_STATE_RECOVERING: return state_e::recovering;
        case ALK_STREAM_QUALITY_STATE_HEALTHY:
        default: return state_e::healthy;
      }
    }

    static availability_e from_alk_availability(uint32_t value) {
      switch (value) {
        case ALK_STREAM_QUALITY_AVAILABILITY_LOW: return availability_e::low;
        case ALK_STREAM_QUALITY_AVAILABILITY_PROBING: return availability_e::probing;
        case ALK_STREAM_QUALITY_AVAILABILITY_RECOVERING: return availability_e::recovering;
        case ALK_STREAM_QUALITY_AVAILABILITY_HIGH:
        default: return availability_e::high;
      }
    }

    static tier_e from_alk_tier(uint32_t value) {
      switch (value) {
        case ALK_STREAM_QUALITY_TIER_FAST: return tier_e::fast;
        case ALK_STREAM_QUALITY_TIER_GENERAL: return tier_e::general;
        case ALK_STREAM_QUALITY_TIER_HD: return tier_e::hd;
        case ALK_STREAM_QUALITY_TIER_BLURAY:
        default: return tier_e::bluray;
      }
    }

    static uint32_t to_alk_reason(reason_e value) {
      switch (value) {
        case reason_e::startup: return ALK_STREAM_QUALITY_REASON_STARTUP;
        case reason_e::recovering: return ALK_STREAM_QUALITY_REASON_RECOVERING;
        case reason_e::random_loss: return ALK_STREAM_QUALITY_REASON_RANDOM_LOSS;
        case reason_e::qos_policer: return ALK_STREAM_QUALITY_REASON_QOS_POLICER;
        case reason_e::delay_congestion: return ALK_STREAM_QUALITY_REASON_DELAY_CONGESTION;
        case reason_e::handover: return ALK_STREAM_QUALITY_REASON_HANDOVER;
        case reason_e::render_deadline: return ALK_STREAM_QUALITY_REASON_RENDER_DEADLINE;
        case reason_e::input_pressure: return ALK_STREAM_QUALITY_REASON_INPUT_PRESSURE;
        case reason_e::audio_pressure: return ALK_STREAM_QUALITY_REASON_AUDIO_PRESSURE;
        case reason_e::motion_pressure: return ALK_STREAM_QUALITY_REASON_MOTION_PRESSURE;
        case reason_e::media_continuity: return ALK_STREAM_QUALITY_REASON_MEDIA_CONTINUITY;
        case reason_e::healthy:
        default: return ALK_STREAM_QUALITY_REASON_HEALTHY;
      }
    }

    static reason_e from_alk_reason(uint32_t value) {
      switch (value) {
        case ALK_STREAM_QUALITY_REASON_STARTUP: return reason_e::startup;
        case ALK_STREAM_QUALITY_REASON_RECOVERING: return reason_e::recovering;
        case ALK_STREAM_QUALITY_REASON_RANDOM_LOSS: return reason_e::random_loss;
        case ALK_STREAM_QUALITY_REASON_QOS_POLICER: return reason_e::qos_policer;
        case ALK_STREAM_QUALITY_REASON_DELAY_CONGESTION: return reason_e::delay_congestion;
        case ALK_STREAM_QUALITY_REASON_HANDOVER: return reason_e::handover;
        case ALK_STREAM_QUALITY_REASON_RENDER_DEADLINE: return reason_e::render_deadline;
        case ALK_STREAM_QUALITY_REASON_INPUT_PRESSURE: return reason_e::input_pressure;
        case ALK_STREAM_QUALITY_REASON_AUDIO_PRESSURE: return reason_e::audio_pressure;
        case ALK_STREAM_QUALITY_REASON_MOTION_PRESSURE: return reason_e::motion_pressure;
        case ALK_STREAM_QUALITY_REASON_MEDIA_CONTINUITY: return reason_e::media_continuity;
        case ALK_STREAM_QUALITY_REASON_HEALTHY:
        default: return reason_e::healthy;
      }
    }

    static uint32_t to_alk_scenario(scenario_e value) {
      switch (value) {
        case scenario_e::startup: return ALK_STREAM_QUALITY_SCENARIO_STARTUP;
        case scenario_e::strong_lan: return ALK_STREAM_QUALITY_SCENARIO_STRONG_LAN;
        case scenario_e::clean_alr: return ALK_STREAM_QUALITY_SCENARIO_CLEAN_ALR;
        case scenario_e::random_loss: return ALK_STREAM_QUALITY_SCENARIO_RANDOM_LOSS;
        case scenario_e::qos_policer: return ALK_STREAM_QUALITY_SCENARIO_QOS_POLICER;
        case scenario_e::delay_congestion: return ALK_STREAM_QUALITY_SCENARIO_DELAY_CONGESTION;
        case scenario_e::handover: return ALK_STREAM_QUALITY_SCENARIO_HANDOVER;
        case scenario_e::local_render: return ALK_STREAM_QUALITY_SCENARIO_LOCAL_RENDER;
        case scenario_e::motion_pressure: return ALK_STREAM_QUALITY_SCENARIO_MOTION_PRESSURE;
        case scenario_e::audio_pressure: return ALK_STREAM_QUALITY_SCENARIO_AUDIO_PRESSURE;
        case scenario_e::input_pressure: return ALK_STREAM_QUALITY_SCENARIO_INPUT_PRESSURE;
        case scenario_e::no_video_delivery: return ALK_STREAM_QUALITY_SCENARIO_NO_VIDEO_DELIVERY;
        case scenario_e::media_continuity: return ALK_STREAM_QUALITY_SCENARIO_MEDIA_CONTINUITY;
        case scenario_e::recovering: return ALK_STREAM_QUALITY_SCENARIO_RECOVERING;
        case scenario_e::healthy:
        default: return ALK_STREAM_QUALITY_SCENARIO_HEALTHY;
      }
    }

    static scenario_e from_alk_scenario(uint32_t value) {
      switch (value) {
        case ALK_STREAM_QUALITY_SCENARIO_STARTUP: return scenario_e::startup;
        case ALK_STREAM_QUALITY_SCENARIO_STRONG_LAN: return scenario_e::strong_lan;
        case ALK_STREAM_QUALITY_SCENARIO_CLEAN_ALR: return scenario_e::clean_alr;
        case ALK_STREAM_QUALITY_SCENARIO_RANDOM_LOSS: return scenario_e::random_loss;
        case ALK_STREAM_QUALITY_SCENARIO_QOS_POLICER: return scenario_e::qos_policer;
        case ALK_STREAM_QUALITY_SCENARIO_DELAY_CONGESTION: return scenario_e::delay_congestion;
        case ALK_STREAM_QUALITY_SCENARIO_HANDOVER: return scenario_e::handover;
        case ALK_STREAM_QUALITY_SCENARIO_LOCAL_RENDER: return scenario_e::local_render;
        case ALK_STREAM_QUALITY_SCENARIO_MOTION_PRESSURE: return scenario_e::motion_pressure;
        case ALK_STREAM_QUALITY_SCENARIO_AUDIO_PRESSURE: return scenario_e::audio_pressure;
        case ALK_STREAM_QUALITY_SCENARIO_INPUT_PRESSURE: return scenario_e::input_pressure;
        case ALK_STREAM_QUALITY_SCENARIO_NO_VIDEO_DELIVERY: return scenario_e::no_video_delivery;
        case ALK_STREAM_QUALITY_SCENARIO_MEDIA_CONTINUITY: return scenario_e::media_continuity;
        case ALK_STREAM_QUALITY_SCENARIO_RECOVERING: return scenario_e::recovering;
        case ALK_STREAM_QUALITY_SCENARIO_HEALTHY:
        default: return scenario_e::healthy;
      }
    }

    static AlkStreamQualityConfig to_alk_config(const config_t &config) {
      AlkStreamQualityConfig result;
      alk_stream_quality_config_init(&result);
      result.baseline_bitrate_kbps = config.baseline_bitrate_kbps;
      result.baseline_fec_percentage = config.baseline_fec_percentage;
      result.max_fec_percentage = config.max_fec_percentage;
      result.startup_bitrate_kbps = config.startup_bitrate_kbps;
      result.ceiling_total_bitrate_kbps = config.ceiling_total_bitrate_kbps;
      result.min_bitrate_kbps = config.min_bitrate_kbps;
      result.baseline_fps = config.baseline_fps;
      result.startup_fps = config.startup_fps;
      result.min_fps = config.min_fps;
      result.frame_width = config.frame_width;
      result.frame_height = config.frame_height;
      result.chroma_sampling_type = config.chroma_sampling_type;
      result.dynamic_range = config.dynamic_range;
      result.runtime_profile_tier_supported = config.runtime_profile_tier_supported;
      result.user_quality_kbps = config.user_quality_kbps;
      result.ideal_demand_kbps = config.ideal_demand_kbps;
      result.fps_needed_kbps = config.fps_needed_kbps;
      return result;
    }

    static AlkStreamQualityFeedback to_alk_feedback(const feedback_t &feedback) {
      AlkStreamQualityFeedback result;
      alk_stream_quality_feedback_init(&result);
      result.duration_ms = feedback.duration_ms;
      result.frames_seen = feedback.frames_seen;
      result.complete_frames = feedback.complete_frames;
      result.recovered_frames = feedback.recovered_frames;
      result.unrecoverable_frames = feedback.unrecoverable_frames;
      result.missing_packets = feedback.missing_packets;
      result.total_packets = feedback.total_packets;
      result.received_packets = feedback.received_packets;
      result.video_bytes = feedback.video_bytes;
      result.rtt_ms = feedback.rtt_ms;
      result.rtt_variance_ms = feedback.rtt_variance_ms;
      result.audio_underruns = feedback.audio_underruns;
      result.decode_queue_depth = feedback.decode_queue_depth;
      result.render_queue_depth = feedback.render_queue_depth;
      result.late_frames = feedback.late_frames;
      result.displayed_frames = feedback.displayed_frames;
      result.visual_stale_frames = feedback.visual_stale_frames;
      result.duplicate_frames = feedback.duplicate_frames;
      result.input_queue_depth = feedback.input_queue_depth;
      result.input_send_latency_us = feedback.input_send_latency_us;
      result.input_ack_latency_us = feedback.input_ack_latency_us;
      result.audio_concealed_ms = feedback.audio_concealed_ms;
      result.late_audio_drops = feedback.late_audio_drops;
      result.audio_plc_ms = feedback.audio_plc_ms;
      result.audio_fade_ms = feedback.audio_fade_ms;
      result.audio_buffer_depth_ms = feedback.audio_buffer_depth_ms;
      result.audio_drift_ppm = feedback.audio_drift_ppm;
      result.frame_area = feedback.frame_area;
      result.dirty_area = feedback.dirty_area;
      if (feedback.full_frame_dirty) {
        result.flags |= ALK_STREAM_QUALITY_FEEDBACK_FLAG_FULL_FRAME_DIRTY;
      }
      result.rfi_requests = feedback.rfi_requests;
      result.waiting_for_rfi_frames = feedback.waiting_for_rfi_frames;
      result.large_frame_fec_skipped = feedback.large_frame_fec_skipped;
      result.local_display_pressure = feedback.local_display_pressure;
      result.delay_gradient_us = feedback.delay_gradient_us;
      result.interarrival_jitter_us = feedback.interarrival_jitter_us;
      result.delay_samples = feedback.delay_samples;
      if (feedback.delay_gradient_valid) {
        result.flags |= ALK_STREAM_QUALITY_FEEDBACK_FLAG_DELAY_GRADIENT_VALID;
      }
      if (feedback.user_input_active) {
        result.flags |= ALK_STREAM_QUALITY_FEEDBACK_FLAG_USER_INPUT_ACTIVE;
      }
      return result;
    }

    static action_t from_alk_decision(const AlkStreamQualityDecision &decision) {
      action_t result {};
      result.changed = (decision.flags & ALK_STREAM_QUALITY_DECISION_FLAG_CHANGED) != 0;
      result.state = from_alk_state(decision.state);
      result.availability = from_alk_availability(decision.availability);
      result.reason = from_alk_reason(decision.reason);
      result.scenario = from_alk_scenario(decision.scenario);
      result.tier = from_alk_tier(decision.tier);
      result.target_bitrate_kbps = decision.target_bitrate_kbps;
      result.fec_percentage = decision.fec_percentage;
      result.pacing_bitrate_kbps = decision.pacing_bitrate_kbps;
      result.target_fps = decision.target_fps;
      result.requested_ceiling_kbps = decision.requested_ceiling_kbps;
      result.effective_ceiling_kbps = decision.effective_ceiling_kbps;
      result.sustainable_estimate_kbps = decision.sustainable_estimate_kbps;
      result.encoding_budget_kbps = decision.encoding_budget_kbps;
      result.fec_budget_kbps = decision.fec_budget_kbps;
      result.packet_loss = decision.packet_loss;
      result.recovered_loss = decision.recovered_loss;
      result.unrecoverable_loss = decision.unrecoverable_loss;
      result.fec_efficiency = decision.fec_efficiency;
      result.pressures.random_loss = decision.pressures.random_loss;
      result.pressures.burst_loss = decision.pressures.burst_loss;
      result.pressures.delay_congestion = decision.pressures.delay_congestion;
      result.pressures.motion = decision.pressures.motion;
      result.pressures.render = decision.pressures.render;
      result.pressures.audio = decision.pressures.audio;
      result.pressures.input = decision.pressures.input;
      result.resolution_scale_percent = decision.resolution_scale_percent;
      result.actual_scale_percent = decision.actual_scale_percent;
      result.chroma_sampling_type = decision.chroma_sampling_type;
      result.dynamic_range = decision.dynamic_range;
      result.quality_tier = decision.quality_tier;
      result.profile_tier_changed = (decision.flags & ALK_STREAM_QUALITY_DECISION_FLAG_PROFILE_TIER_CHANGED) != 0;
      result.profile_tier_deferred = (decision.flags & ALK_STREAM_QUALITY_DECISION_FLAG_PROFILE_TIER_DEFERRED) != 0;
      result.profile_tier_supported = (decision.flags & ALK_STREAM_QUALITY_DECISION_FLAG_PROFILE_TIER_SUPPORTED) != 0;
      result.runtime_scale_applied = (decision.flags & ALK_STREAM_QUALITY_DECISION_FLAG_RUNTIME_SCALE_APPLIED) != 0;
      result.rfi_limited = (decision.flags & ALK_STREAM_QUALITY_DECISION_FLAG_RFI_LIMITED) != 0;
      result.request_idr = (decision.flags & ALK_STREAM_QUALITY_DECISION_FLAG_REQUEST_IDR) != 0;
      result.congestion_anti_spiral = (decision.flags & ALK_STREAM_QUALITY_DECISION_FLAG_CONGESTION_ANTI_SPIRAL) != 0;
      result.burst_safe_mode = (decision.flags & ALK_STREAM_QUALITY_DECISION_FLAG_BURST_SAFE_MODE) != 0;
      result.unsafe_ceiling_active = (decision.flags & ALK_STREAM_QUALITY_DECISION_FLAG_UNSAFE_CEILING_ACTIVE) != 0;
      result.unsafe_fps_ceiling_active = (decision.flags & ALK_STREAM_QUALITY_DECISION_FLAG_UNSAFE_FPS_CEILING_ACTIVE) != 0;
      result.recovery_hold_remaining = decision.recovery_hold_remaining;
      result.rtt_gradient_us = decision.rtt_gradient_us;
      result.owd_gradient_us = decision.owd_gradient_us;
      result.owd_pressure = decision.owd_pressure;
      return result;
    }
  }  // namespace

  runtime_fps_apply_decision_t
  runtime_fps_apply_decision(int last_fps, int target_fps, int elapsed_ms_since_last_apply) {
    const auto decision = alk_stream_quality_runtime_fps_apply_decision(last_fps, target_fps, elapsed_ms_since_last_apply);
    return {
      .target_changed = decision.target_changed,
      .apply = decision.apply,
      .deferred = decision.deferred,
      .cooldown_ms = decision.cooldown_ms,
    };
  }

  fps_probe_backoff_t
  fps_probe_backoff_after_failed_recovery(int previous_fps,
                                          int last_probe_fps,
                                          int target_fps,
                                          bool pressure_after_probe,
                                          int previous_failed_probe_count,
                                          int previous_hold_windows) {
    const auto backoff = alk_stream_quality_fps_probe_backoff_after_failed_recovery(previous_fps,
                                                                            last_probe_fps,
                                                                            target_fps,
                                                                            pressure_after_probe,
                                                                            previous_failed_probe_count,
                                                                            previous_hold_windows);
    return {
      .failed_probe_count = backoff.failed_probe_count,
      .recovery_hold_windows = backoff.recovery_hold_windows,
      .recovery_probe_interval_windows = backoff.recovery_probe_interval_windows,
    };
  }

  controller_t::controller_t():
      module_(alk_stream_quality_adaptive_controller_module_create()) {
    alk_stream_quality_decision_init(&last_decision_);
  }

  controller_t::~controller_t() {
    alk_stream_quality_adaptive_controller_module_destroy(module_);
  }

  controller_t::controller_t(controller_t &&other) noexcept:
      module_(std::exchange(other.module_, nullptr)),
      last_decision_(other.last_decision_) {}

  controller_t &
  controller_t::operator=(controller_t &&other) noexcept {
    if (this != &other) {
      alk_stream_quality_adaptive_controller_module_destroy(module_);
      module_ = std::exchange(other.module_, nullptr);
      last_decision_ = other.last_decision_;
    }
    return *this;
  }

  void
  controller_t::configure(config_t config) {
    if (!module_) {
      module_ = alk_stream_quality_adaptive_controller_module_create();
    }
    AlkSessionComponentManifest manifest;
    alk_stream_quality_adaptive_controller_manifest(&manifest);
    const auto alk_config = to_alk_config(config);
    if (alk_stream_quality_adaptive_controller_module_configure(module_, &alk_config) &&
        alk_stream_quality_adaptive_controller_module_start(module_)) {
      alk_stream_quality_adaptive_controller_module_get_decision(module_, &last_decision_);
      log_module_activation(manifest, config);
    }
  }

  action_t
  controller_t::on_feedback(const feedback_t &feedback) {
    if (!module_) {
      module_ = alk_stream_quality_adaptive_controller_module_create();
    }
    const auto alk_feedback = to_alk_feedback(feedback);
    alk_stream_quality_adaptive_controller_module_update(module_, &alk_feedback, &last_decision_);
    return from_alk_decision(last_decision_);
  }

  int controller_t::current_bitrate_kbps() const { return last_decision_.target_bitrate_kbps; }
  int controller_t::current_fec_percentage() const { return last_decision_.fec_percentage; }
  int controller_t::pacing_bitrate_kbps() const { return last_decision_.pacing_bitrate_kbps; }
  int controller_t::current_fps() const { return last_decision_.target_fps; }
  int controller_t::requested_ceiling_kbps() const { return last_decision_.requested_ceiling_kbps; }
  int controller_t::effective_ceiling_kbps() const { return last_decision_.effective_ceiling_kbps; }
  int controller_t::sustainable_estimate_kbps() const { return last_decision_.sustainable_estimate_kbps; }
  state_e controller_t::state() const { return from_alk_state(last_decision_.state); }

  std::uint32_t
  infer_local_display_pressure(const feedback_t &feedback) {
    const auto alk_feedback = to_alk_feedback(feedback);
    return alk_stream_quality_infer_local_display_pressure(&alk_feedback);
  }

  const char *reason_name(reason_e reason) { return alk_stream_quality_reason_name(to_alk_reason(reason)); }
  const char *scenario_name(scenario_e scenario) { return alk_stream_quality_scenario_name(to_alk_scenario(scenario)); }
  const char *availability_name(availability_e availability) {
    switch (availability) {
      case availability_e::high: return alk_stream_quality_availability_name(ALK_STREAM_QUALITY_AVAILABILITY_HIGH);
      case availability_e::low: return alk_stream_quality_availability_name(ALK_STREAM_QUALITY_AVAILABILITY_LOW);
      case availability_e::probing: return alk_stream_quality_availability_name(ALK_STREAM_QUALITY_AVAILABILITY_PROBING);
      case availability_e::recovering: return alk_stream_quality_availability_name(ALK_STREAM_QUALITY_AVAILABILITY_RECOVERING);
    }
    return "unknown";
  }
  const char *tier_name(tier_e tier) {
    switch (tier) {
      case tier_e::fast: return alk_stream_quality_tier_name(ALK_STREAM_QUALITY_TIER_FAST);
      case tier_e::general: return alk_stream_quality_tier_name(ALK_STREAM_QUALITY_TIER_GENERAL);
      case tier_e::hd: return alk_stream_quality_tier_name(ALK_STREAM_QUALITY_TIER_HD);
      case tier_e::bluray: return alk_stream_quality_tier_name(ALK_STREAM_QUALITY_TIER_BLURAY);
    }
    return "unknown";
  }
}  // namespace stream_quality
