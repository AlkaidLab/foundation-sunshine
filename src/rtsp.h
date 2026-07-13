/**
 * @file src/rtsp.h
 * @brief Declarations for RTSP streaming.
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <string_view>

#include <boost/process/v1.hpp>

#include "crypto.h"
#include "launch_session_manager.h"
#include "thread_safe.h"

namespace rtsp_stream {
  constexpr auto RTSP_SETUP_PORT = 21;

  struct client_session_cancel_result_t {
    std::size_t cancelled_sessions {};
    std::size_t remaining_sessions {};
  };

  struct launch_session_t {
    uint32_t id;

    crypto::aes_t gcm_key;
    crypto::aes_t iv;

    std::string av_ping_payload;
    uint32_t control_connect_data;
    std::string client_cert_uuid;
    std::string rtsp_peer_address;

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
    bool continuous_audio;
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

  launch_ticket_register_e
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

  /**
   * @brief Get the number of bounded launch tickets awaiting RTSP activation.
   */
  int
  pending_session_count();

  /**
   * @brief Terminates all running streaming sessions.
   */
  void
  terminate_sessions();

  /**
   * @brief Terminates sessions owned by one authenticated client.
   */
  client_session_cancel_result_t
  cancel_client_sessions(std::string_view client_cert_uuid);

  /**
   * @brief Runs the RTSP server loop.
   */
  void start();
}  // namespace rtsp_stream
