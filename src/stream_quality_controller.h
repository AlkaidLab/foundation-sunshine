#pragma once

#include <cstdint>

#include <alkaidlab/modules/stream_quality/adaptive_controller.h>

namespace stream_quality {
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
    qos_policer,
    delay_congestion,
    handover,
    render_deadline,
    input_pressure,
    audio_pressure,
    motion_pressure,
    media_continuity,
  };

  enum class scenario_e {
    startup,
    strong_lan,
    clean_alr,
    random_loss,
    qos_policer,
    delay_congestion,
    handover,
    local_render,
    motion_pressure,
    audio_pressure,
    input_pressure,
    no_video_delivery,
    media_continuity,
    recovering,
    healthy,
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
    int min_bitrate_kbps = 500;
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
    std::int32_t delay_gradient_us = 0;
    std::uint32_t interarrival_jitter_us = 0;
    std::uint32_t delay_samples = 0;
    bool delay_gradient_valid = false;
    bool user_input_active = false;
  };

  struct action_t {
    bool changed = false;
    state_e state = state_e::healthy;
    availability_e availability = availability_e::probing;
    reason_e reason = reason_e::healthy;
    scenario_e scenario = scenario_e::healthy;
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
    bool congestion_anti_spiral = false;
    bool burst_safe_mode = false;
    bool unsafe_ceiling_active = false;
    bool unsafe_fps_ceiling_active = false;
    int recovery_hold_remaining = 0;
    int rtt_gradient_us = 0;
    int owd_gradient_us = 0;
    double owd_pressure = 0.0;
  };

  class controller_t {
  public:
    static constexpr int max_fec_percentage = 100;

    controller_t();
    ~controller_t();
    controller_t(const controller_t &) = delete;
    controller_t &operator=(const controller_t &) = delete;
    controller_t(controller_t &&other) noexcept;
    controller_t &operator=(controller_t &&other) noexcept;

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
    AlkStreamQualityAdaptiveControllerModule *module_ = nullptr;
    AlkStreamQualityDecision last_decision_ {};
  };

  const char *
  reason_name(reason_e reason);

  const char *
  scenario_name(scenario_e scenario);

  std::uint32_t
  infer_local_display_pressure(const feedback_t &feedback);

  const char *
  availability_name(availability_e availability);

  const char *
  tier_name(tier_e tier);
}  // namespace stream_quality
