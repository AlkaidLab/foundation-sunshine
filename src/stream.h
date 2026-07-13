/**
 * @file src/stream.h
 * @brief Declarations for the streaming protocols.
 */
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio.hpp>

#include "audio.h"
#include "crypto.h"
#include "video.h"

namespace rtsp_stream {
  struct launch_session_t;
}

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

    enum class state_e : int {
      STOPPED,  ///< The session is stopped
      STOPPING,  ///< The session is stopping
      STARTING,  ///< The session is starting
      RUNNING,  ///< The session is running
    };

    struct lifecycle_snapshot_t {
      state_e state;
      stop_reason_e stop_reason;
    };

    class lifecycle_t {
    public:
      explicit lifecycle_t(state_e initial_state = state_e::STOPPED) noexcept:
          _state(initial_state) {}

      state_e
      state() const noexcept {
        return _state.load(std::memory_order_acquire);
      }

      void
      set_state(state_e state) {
        std::lock_guard lock(_mutex);
        _state.store(state, std::memory_order_release);
      }

      bool
      request_stop(stop_reason_e reason) {
        std::lock_guard lock(_mutex);
        if (_state.load(std::memory_order_relaxed) != state_e::RUNNING) {
          return false;
        }

        _stop_reason = reason;
        _state.store(state_e::STOPPING, std::memory_order_release);
        return true;
      }

      lifecycle_snapshot_t
      snapshot() const {
        std::lock_guard lock(_mutex);
        return {
          _state.load(std::memory_order_relaxed),
          _stop_reason,
        };
      }

    private:
      mutable std::mutex _mutex;
      std::atomic<state_e> _state;
      stop_reason_e _stop_reason { stop_reason_e::none };
    };
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
