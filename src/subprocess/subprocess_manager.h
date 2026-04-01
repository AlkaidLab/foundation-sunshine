/**
 * @file src/subprocess/subprocess_manager.h
 * @brief Subprocess lifecycle management for streaming data plane.
 *
 * This module manages the lifecycle of subprocess workers that handle
 * video/audio capture, encoding, and network transmission.
 */
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "ipc_pipe.h"
#include "ipc_protocol.h"
#include "subprocess_config.h"

namespace subprocess {

  /**
   * @brief Subprocess state.
   */
  enum class state_e {
    stopped,      ///< Not running
    starting,     ///< Starting up
    ready,        ///< Ready to stream
    streaming,    ///< Actively streaming
    stopping,     ///< Shutting down
    error,        ///< Error state
  };

  /**
   * @brief Convert state to string.
   */
  const char *
  state_to_string(state_e state);

  /**
   * @brief Callback for subprocess status changes.
   */
  using status_callback_t = std::function<void(state_e new_state, int error_code, const std::string &error_msg)>;

  /**
   * @brief Session configuration for subprocess initialization.
   */
  struct session_config_t {
    // Session identification
    uint32_t session_id;
    std::string client_name;

    // Video configuration
    int width;
    int height;
    int framerate;
    int bitrate_kbps;
    int slices_per_frame;
    int num_ref_frames;
    int encoder_csc_mode;
    int video_format;      // 0=H264, 1=HEVC, 2=AV1
    int dynamic_range;
    int chroma_sampling;
    int enable_intra_refresh;

    // Audio configuration
    int audio_channels;
    int audio_mask;
    int audio_packet_duration;
    bool audio_high_quality;
    bool audio_host_audio;

    // Network configuration
    int packet_size;
    int min_fec_packets;
    int fec_percentage;

    // Encryption
    uint8_t encryption_flags;
    std::array<uint8_t, 16> gcm_key;
    std::array<uint8_t, 16> iv;

    // Display
    std::string display_name;
  };

  /**
   * @brief Manages a single subprocess worker.
   */
  class subprocess_worker_t {
  public:
    subprocess_worker_t();
    ~subprocess_worker_t();

    // Non-copyable
    subprocess_worker_t(const subprocess_worker_t &) = delete;
    subprocess_worker_t &operator=(const subprocess_worker_t &) = delete;

    /**
     * @brief Start the subprocess with given configuration.
     * @param config Session configuration.
     * @param status_callback Callback for status changes.
     * @return true on success, false on failure.
     */
    bool
    start(const session_config_t &config, status_callback_t status_callback);

    /**
     * @brief Stop the subprocess.
     * @param wait_timeout_ms Timeout to wait for graceful shutdown.
     */
    void
    stop(int wait_timeout_ms = 5000);

    /**
     * @brief Request IDR frame from encoder.
     */
    void
    request_idr_frame();

    /**
     * @brief Change encoding bitrate.
     * @param new_bitrate_kbps New bitrate in Kbps.
     */
    void
    change_bitrate(int new_bitrate_kbps);

    /**
     * @brief Invalidate reference frames.
     * @param first_frame First frame to invalidate.
     * @param last_frame Last frame to invalidate.
     */
    void
    invalidate_ref_frames(int64_t first_frame, int64_t last_frame);

    /**
     * @brief Get current state.
     */
    state_e
    get_state() const {
      return state_.load();
    }

    /**
     * @brief Check if subprocess is running.
     */
    bool
    is_running() const {
      auto s = state_.load();
      return s == state_e::starting || s == state_e::ready || s == state_e::streaming;
    }

#ifdef _WIN32
    /**
     * @brief Transfer socket to subprocess (Windows).
     * @param socket_type 0=video, 1=audio, 2=control
     * @param socket_handle Native socket handle (SOCKET).
     * @param remote_addr Remote address.
     * @param remote_port Remote port.
     * @return true on success.
     */
    bool
    transfer_socket(uint8_t socket_type, uintptr_t socket_handle,
                    const uint8_t *remote_addr, uint8_t addr_family, uint16_t remote_port);
#endif

  private:
    /**
     * @brief Handle messages from subprocess.
     */
    bool
    handle_message(const ipc::message_header_t &header, const std::vector<uint8_t> &payload);

    /**
     * @brief Heartbeat monitoring thread.
     */
    void
    heartbeat_thread();

    /**
     * @brief Launch the subprocess executable.
     */
    bool
    launch_process();

    /**
     * @brief Terminate the subprocess.
     */
    void
    terminate_process();

    // Configuration
    session_config_t config_;
    status_callback_t status_callback_;

    // State
    std::atomic<state_e> state_ { state_e::stopped };

    // IPC
    std::unique_ptr<ipc::pipe_server_t> ipc_server_;

    // Process handle
    void *process_handle_ = nullptr;

    // Heartbeat
    std::thread heartbeat_thread_;
    std::atomic<bool> heartbeat_running_ { false };
    std::atomic<std::chrono::steady_clock::time_point> last_heartbeat_;

    // Mutex for thread safety
    mutable std::mutex mutex_;
  };

  /**
   * @brief Global subprocess manager for multiple workers.
   */
  class subprocess_manager_t {
  public:
    static subprocess_manager_t &
    instance();

    /**
     * @brief Create a new subprocess worker for a session.
     * @param session_id Session ID.
     * @return Shared pointer to worker, or nullptr on failure.
     */
    std::shared_ptr<subprocess_worker_t>
    create_worker(uint32_t session_id);

    /**
     * @brief Get an existing worker by session ID.
     * @param session_id Session ID.
     * @return Shared pointer to worker, or nullptr if not found.
     */
    std::shared_ptr<subprocess_worker_t>
    get_worker(uint32_t session_id);

    /**
     * @brief Remove a worker by session ID.
     * @param session_id Session ID.
     */
    void
    remove_worker(uint32_t session_id);

    /**
     * @brief Stop all workers.
     */
    void
    stop_all();

  private:
    subprocess_manager_t() = default;
    ~subprocess_manager_t() = default;

    std::mutex mutex_;
    std::unordered_map<uint32_t, std::shared_ptr<subprocess_worker_t>> workers_;
  };

}  // namespace subprocess
