#pragma once

#include <cstdint>

namespace weak_net {
  enum class state_e {
    healthy,
    constrained,
    crisis,
    recovering,
  };

  enum class availability_e {
    high,
    low,
    probing,
    recovering,
  };

  enum class tier_e {
    fast,
    general,
    hd,
    bluray,
  };

  enum class reason_e {
    startup,
    healthy,
    recovering,
    random_loss,
    delay_congestion,
    render_deadline,
    input_pressure,
    audio_pressure,
    motion_pressure,
  };

  struct pressure_signals_t {
    double random_loss = 0.0;
    double burst_loss = 0.0;
    double delay_congestion = 0.0;
    double motion = 0.0;
    double render = 0.0;
    double audio = 0.0;
    double input = 0.0;
  };

  struct runtime_targets_t {
    int encoding_bitrate_kbps = 0;
    int fec_percentage = 0;
    int pacing_bitrate_kbps = 0;
    int fps = 0;
    int resolution_scale_percent = 100;
    int chroma_sampling_type = -1;
    int dynamic_range = -1;
  };

  struct applied_runtime_state_t {
    runtime_targets_t targets;
    bool bitrate_applied = false;
    bool fec_applied = false;
    bool fps_applied = false;
    bool profile_tier_active = false;
  };

  struct runtime_fps_apply_decision_t {
    bool target_changed = false;
    bool apply = false;
    bool deferred = false;
    int cooldown_ms = 0;
  };

  struct fps_probe_backoff_t {
    int failed_probe_count = 0;
    int recovery_hold_windows = 0;
    int recovery_probe_interval_windows = 1;
  };

  struct config_t {
    int baseline_bitrate_kbps = 0;
    int baseline_fec_percentage = 0;
    int max_fec_percentage = 100;
    int startup_bitrate_kbps = 0;
    int ceiling_total_bitrate_kbps = 0;
    int min_bitrate_kbps = 1500;
    int baseline_fps = 60;
    int startup_fps = 0;
    int min_fps = 0;
    int frame_width = 0;
    int frame_height = 0;
    int chroma_sampling_type = -1;
    int dynamic_range = -1;
    bool runtime_profile_tier_supported = false;
    int user_quality_kbps = 0;
    int ideal_demand_kbps = 0;
    int fps_needed_kbps = 0;
  };

  runtime_fps_apply_decision_t
  runtime_fps_apply_decision(int last_fps, int target_fps, int elapsed_ms_since_last_apply);

  fps_probe_backoff_t
  fps_probe_backoff_after_failed_recovery(int previous_fps,
                                          int last_probe_fps,
                                          int target_fps,
                                          bool pressure_after_probe,
                                          int previous_failed_probe_count,
                                          int previous_hold_windows);

  struct feedback_t {
    std::uint32_t duration_ms = 0;
    std::uint32_t frames_seen = 0;
    std::uint32_t complete_frames = 0;
    std::uint32_t recovered_frames = 0;
    std::uint32_t unrecoverable_frames = 0;
    std::uint32_t missing_packets = 0;
    std::uint32_t total_packets = 0;
    std::uint32_t received_packets = 0;
    std::uint32_t video_bytes = 0;
    std::uint32_t rtt_ms = 0;
    std::uint32_t rtt_variance_ms = 0;
    std::uint32_t audio_underruns = 0;
    std::uint32_t decode_queue_depth = 0;
    std::uint32_t render_queue_depth = 0;
    std::uint32_t late_frames = 0;
    std::uint32_t displayed_frames = 0;
    std::uint32_t visual_stale_frames = 0;
    std::uint32_t duplicate_frames = 0;
    std::uint32_t input_queue_depth = 0;
    std::uint32_t input_send_latency_us = 0;
    std::uint32_t input_ack_latency_us = 0;
    std::uint32_t audio_concealed_ms = 0;
    std::uint32_t late_audio_drops = 0;
    std::uint32_t audio_plc_ms = 0;
    std::uint32_t audio_fade_ms = 0;
    std::uint32_t audio_buffer_depth_ms = 0;
    std::int32_t audio_drift_ppm = 0;
    std::uint64_t frame_area = 0;
    std::uint64_t dirty_area = 0;
    bool full_frame_dirty = false;
    std::uint32_t rfi_requests = 0;
    std::uint32_t waiting_for_rfi_frames = 0;
    std::uint32_t large_frame_fec_skipped = 0;
    std::uint32_t local_display_pressure = 0;
  };

  struct action_t {
    bool changed = false;
    state_e state = state_e::healthy;
    availability_e availability = availability_e::probing;
    reason_e reason = reason_e::healthy;
    tier_e tier = tier_e::bluray;
    int target_bitrate_kbps = 0;
    int fec_percentage = 0;
    int pacing_bitrate_kbps = 0;
    int target_fps = 0;
    int requested_ceiling_kbps = 0;
    int effective_ceiling_kbps = 0;
    int sustainable_estimate_kbps = 0;
    int encoding_budget_kbps = 0;
    int fec_budget_kbps = 0;
    double packet_loss = 0.0;
    double recovered_loss = 0.0;
    double unrecoverable_loss = 0.0;
    double fec_efficiency = 0.0;
    pressure_signals_t pressures;
    int resolution_scale_percent = 100;
    int actual_scale_percent = 100;
    int chroma_sampling_type = -1;
    int dynamic_range = -1;
    int quality_tier = 0;
    bool profile_tier_changed = false;
    bool profile_tier_deferred = false;
    bool profile_tier_supported = false;
    bool runtime_scale_applied = false;
    bool rfi_limited = false;
    bool request_idr = false;
  };

  class controller_t {
  public:
    static constexpr int max_fec_percentage = 100;

    void configure(config_t config);
    action_t on_feedback(const feedback_t &feedback);

    int current_bitrate_kbps() const;
    int current_fec_percentage() const;
    int pacing_bitrate_kbps() const;
    int current_fps() const;
    int requested_ceiling_kbps() const;
    int effective_ceiling_kbps() const;
    int sustainable_estimate_kbps() const;
    state_e state() const;

  private:
    double ewma_loss_ = 0.0;
    double ewma_unrecoverable_ = 0.0;
    double ewma_jitter_ = 0.0;
    double ewma_deadline_pressure_ = 0.0;
    double ewma_input_pressure_ = 0.0;
    double ewma_audio_pressure_ = 0.0;
    double ewma_motion_pressure_ = 0.0;
    double ewma_delay_pressure_ = 0.0;
    double ewma_burst_pressure_ = 0.0;
    int stable_windows_ = 0;
    int video_deadline_windows_ = 0;
    int fps_adjust_cooldown_windows_ = 0;
    int fps_recovery_hold_windows_ = 0;
    int fps_probe_interval_windows_ = 1;
    int failed_fps_probe_windows_ = 0;
    int last_recovery_probe_fps_ = 0;
    int profile_tier_cooldown_windows_ = 0;
    int media_recovery_cooldown_windows_ = 0;
    int bitrate_probe_hold_windows_ = 0;
    int no_video_delivery_windows_ = 0;
    int bitrate_plateau_kbps_ = 0;
    int last_probe_base_bitrate_kbps_ = 0;
    int last_probe_target_bitrate_kbps_ = 0;
    double last_probe_displayed_ratio_ = 0.0;
    double last_probe_displayed_fps_ratio_ = 0.0;
    double last_probe_render_pressure_ = 0.0;
    double last_probe_delay_pressure_ = 0.0;
    int current_bitrate_kbps_ = 0;
    int current_fec_percentage_ = 0;
    int pacing_bitrate_kbps_ = 0;
    int current_fps_ = 0;
    int current_resolution_scale_percent_ = 100;
    int current_chroma_sampling_type_ = -1;
    int current_dynamic_range_ = -1;
    int current_quality_tier_ = 0;
    tier_e current_tier_ = tier_e::bluray;
    availability_e current_availability_ = availability_e::probing;
    int motion_crisis_windows_ = 0;
    int motion_crisis_guard_windows_ = 0;
    int motion_crisis_recovery_windows_ = 0;
    int requested_ceiling_kbps_ = 0;
    int effective_ceiling_kbps_ = 0;
    int sustainable_estimate_kbps_ = 0;
    bool sustainable_limit_active_ = false;
    int idr_cooldown_windows_ = 0;
    int audio_cooldown_windows_ = 0;
    config_t config_;
    state_e state_ = state_e::healthy;
    bool configured_ = false;
  };

  const char *
  reason_name(reason_e reason);

  std::uint32_t
  infer_local_display_pressure(const feedback_t &feedback);

  const char *
  availability_name(availability_e availability);

  const char *
  tier_name(tier_e tier);
}  // namespace weak_net
