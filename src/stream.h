/**
 * @file src/stream.h
 * @brief Declarations for the streaming protocols.
 */
#pragma once
#include <cstdint>
#include <utility>
#include <vector>
#include <string>

#include <boost/asio.hpp>

#include "audio.h"
#include "crypto.h"
#include "video.h"

namespace stream {
  constexpr auto VIDEO_STREAM_PORT = 9;
  constexpr auto CONTROL_PORT = 10;
  constexpr auto AUDIO_STREAM_PORT = 11;
  constexpr auto MIC_STREAM_PORT = 12;  // Port for microphone streaming

  struct session_t;
  struct config_t {
    audio::config_t audio;
    video::config_t monitor;

    int packetsize;
    int minRequiredFecPackets;
    int mlFeatureFlags;
    int controlProtocolType;
    int audioQosType;
    int videoQosType;

    uint32_t encryptionFlagsEnabled;

    std::optional<int> gcmap;
  };

  namespace session {
    enum class stop_reason_e : int {
      none,
      control_disconnect,
      control_timeout,
      protocol_error,
      video_ended,
      audio_ended,
      host_terminate,
    };

    const char *
    stop_reason_name(stop_reason_e reason);
  }  // namespace session

  // Session information structure for API responses
  struct session_info_t {
    std::string client_name;
    std::string client_uuid;
    std::string client_address;
    std::string state;
    std::string stop_reason;
    uint32_t session_id;
    std::int64_t uptime_ms;
    std::int64_t control_idle_ms;
    std::int64_t video_idle_ms;
    std::int64_t audio_idle_ms;
    bool control_connected;
    int width;
    int height;
    int fps;
    int bitrate;  // Current bitrate in Kbps
    bool host_audio;
    bool enable_hdr;
    bool enable_mic;
    std::string app_name;
    int app_id;
  };

  namespace session {
    enum class state_e : int {
      STOPPED,  ///< The session is stopped
      STOPPING,  ///< The session is stopping
      STARTING,  ///< The session is starting
      RUNNING,  ///< The session is running
    };

    std::shared_ptr<session_t>
    alloc(config_t &config, rtsp_stream::launch_session_t &launch_session);
    int
    start(session_t &session, const std::string &addr_string);
    void
    stop(session_t &session, stop_reason_e reason = stop_reason_e::none);
    void
    join(session_t &session);
    state_e
    state(session_t &session);

    bool
    has_active_video_sessions();
    


    /**
     * @brief Send dynamic parameter change event to a specific client session.
     * @param client_name The name of the client to target.
     * @param param The dynamic parameter to change.
     * @return true if the event was sent successfully, false otherwise.
     */
    bool
    change_dynamic_param_for_client(const std::string &client_name, const video::dynamic_param_t &param);

    /**
     * @brief Get information about all active sessions.
     * @return Vector of session information.
     */
    std::vector<session_info_t>
    get_all_sessions_info();
  }  // namespace session
}  // namespace stream
