#pragma once

#include <cstdint>

namespace weak_net {
  enum class state_e {
    healthy,
    constrained,
    crisis,
    recovering,
  };

  struct config_t {
    int baseline_bitrate_kbps = 0;
    int baseline_fec_percentage = 0;
    int max_fec_percentage = 35;
    int startup_bitrate_kbps = 0;
    int ceiling_total_bitrate_kbps = 0;
    int min_bitrate_kbps = 1500;
    int baseline_fps = 60;
    int startup_fps = 0;
    int min_fps = 0;
  };

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
    std::uint32_t input_queue_depth = 0;
    std::uint32_t input_send_latency_us = 0;
    std::uint32_t input_ack_latency_us = 0;
  };

  struct action_t {
    bool changed = false;
    state_e state = state_e::healthy;
    int target_bitrate_kbps = 0;
    int fec_percentage = 0;
    int pacing_bitrate_kbps = 0;
    int target_fps = 0;
    bool request_idr = false;
  };

  class controller_t {
  public:
    static constexpr int max_fec_percentage = 35;

    void configure(config_t config);
    action_t on_feedback(const feedback_t &feedback);

    int current_bitrate_kbps() const;
    int current_fec_percentage() const;
    int pacing_bitrate_kbps() const;
    int current_fps() const;
    state_e state() const;

  private:
    double ewma_loss_ = 0.0;
    double ewma_unrecoverable_ = 0.0;
    double ewma_jitter_ = 0.0;
    double ewma_deadline_pressure_ = 0.0;
    double ewma_input_pressure_ = 0.0;
    double ewma_audio_pressure_ = 0.0;
    int stable_windows_ = 0;
    int video_deadline_windows_ = 0;
    int fps_adjust_cooldown_windows_ = 0;
    int current_bitrate_kbps_ = 0;
    int current_fec_percentage_ = 0;
    int pacing_bitrate_kbps_ = 0;
    int current_fps_ = 0;
    int idr_cooldown_windows_ = 0;
    int audio_cooldown_windows_ = 0;
    config_t config_;
    state_e state_ = state_e::healthy;
    bool configured_ = false;
  };
}  // namespace weak_net
