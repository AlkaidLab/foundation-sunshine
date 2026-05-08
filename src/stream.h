/**
 * @file src/stream.h
 * @brief Declarations for the streaming protocols.
 */
#pragma once
#include <cstdint>
#include <utility>
#include <vector>
#include <string>
#include <string_view>

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
    std::uint64_t mlFeatureFlags2;
    int controlProtocolType;
    int audioQosType;
    int videoQosType;

    uint32_t encryptionFlagsEnabled;

    std::optional<int> gcmap;
  };

  // Session information structure for API responses
  struct session_info_t {
    uint64_t runtime_id;
    uint32_t launch_session_id;
    uint32_t control_generation;
    std::string client_cert_uuid;
    std::string client_unique_id;
    std::string client_name;
    std::string client_address;
    std::string state;
    uint32_t session_id;
    bool trusted_client_identity;
    int width;
    int height;
    int fps;
    int bitrate;  // Current bitrate in Kbps
    bool host_audio;
    bool enable_hdr;
    bool enable_mic;
    bool input_owner;
    uint64_t input_owner_runtime_id;
    bool mic_owner;
    uint64_t mic_owner_runtime_id;
    bool clipboard_owner;
    uint64_t clipboard_owner_runtime_id;
    bool display_owner;
    uint64_t display_owner_runtime_id;
    bool dynamic_quality_owner;
    uint64_t dynamic_quality_owner_runtime_id;
    std::string display_resource_scope;
    std::string display_allocation_mode;
    uint32_t display_resource_slot;
    bool dedicated_display;
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
    stop(session_t &session);
    void
    join(session_t &session);
    state_e
    state(session_t &session);
    std::uint32_t
    teardown_count();

    struct stop_sessions_result_t {
      std::size_t matched {};
      std::size_t stopped {};
      std::size_t starting {};
      std::size_t already_stopping {};
    };

    stop_sessions_result_t
    stop_sessions_for_client(const std::string &client_cert_uuid,
                             const std::string &client_address,
                             std::string_view reason);
    


    /**
     * @brief Send dynamic parameter change event to a specific client session.
     * @param client_name The name of the client to target.
     * @param param The dynamic parameter to change.
     * @return true if the event was sent successfully, false otherwise.
     */
    bool
    change_dynamic_param_for_client(const std::string &client_name, const video::dynamic_param_t &param);

    /**
     * @brief Send dynamic parameter change event to a specific runtime session.
     * @param runtime_id The runtime session ID to target.
     * @param param The dynamic parameter to change.
     * @return true if the event was sent successfully, false otherwise.
     */
    bool
    change_dynamic_param_for_runtime(std::uint64_t runtime_id, const video::dynamic_param_t &param);

    /**
     * @brief Get information about all active sessions.
     * @return Vector of session information.
     */
    std::vector<session_info_t>
    get_all_sessions_info();
  }  // namespace session
}  // namespace stream
