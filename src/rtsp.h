/**
 * @file src/rtsp.h
 * @brief Declarations for RTSP streaming.
 */
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

#include <boost/process/v1.hpp>

#include "crypto.h"
#include "session_runtime.h"
#include "thread_safe.h"

namespace rtsp_stream {
  constexpr auto RTSP_SETUP_PORT = 21;

  struct launch_session_t {
    uint32_t id;

    crypto::aes_t gcm_key;
    crypto::aes_t iv;

    std::string av_ping_payload;
    uint32_t control_connect_data;
    session_runtime::identity_t identity;
    std::string rtsp_peer_address;
    std::chrono::steady_clock::time_point pending_since;

    boost::process::v1::environment env;

    bool host_audio;
    std::string unique_id;
    std::string client_name;
    int width;
    int height;
    int fps;
    int gcmap;
    int appid;
    int surround_info;
    std::string surround_params;
    bool enable_hdr;
    bool enable_sops;
    bool enable_mic;
    bool use_vdd;
    int custom_screen_mode;
    float max_nits;
    float min_nits;
    float max_full_nits;

    std::optional<crypto::cipher::gcm_t> rtsp_cipher;
    std::string rtsp_url_scheme;
    uint32_t rtsp_iv_counter;

    // 跟踪已设置的流类型
    bool setup_video { false };
    bool setup_audio { false };
    bool setup_control { false };
    bool setup_mic { false };
    bool control_only { false };
  };

  void
  launch_session_raise(std::shared_ptr<launch_session_t> launch_session);

  /**
   * @brief Clear state for the specified launch session.
   * @param launch_session_id The ID of the session to clear.
   */
  void
  launch_session_clear(uint32_t launch_session_id);

  /**
   * @brief Get the number of active sessions.
   * @return Count of active sessions.
   */
  int
  session_count();

  std::uint64_t
  foundation_streaming_feature_flags2();

  int
  effective_stream_fec_percentage_for_client(int configured_fec_percentage, int ml_feature_flags);

  int
  adaptive_stream_max_fec_percentage_for_client(int configured_fec_percentage, int ml_feature_flags);

  std::int64_t
  adjust_configured_video_bitrate_kbps(std::int64_t configured_bitrate_kbps,
                                       int fec_percentage,
                                       bool high_quality_audio,
                                       int audio_channels);

  std::int64_t
  video_quality_ceiling_bitrate_kbps(std::int64_t configured_bitrate_kbps,
                                     int fec_percentage,
                                     bool high_quality_audio,
                                     int audio_channels,
                                     int ml_feature_flags);

  /**
   * @brief Terminates all running streaming sessions.
   */
  void
  terminate_sessions();

  /**
   * @brief Runs the RTSP server loop.
   */
  void start();
}  // namespace rtsp_stream
