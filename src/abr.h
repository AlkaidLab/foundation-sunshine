/**
 * @file src/abr.h
 * @brief Adaptive Bitrate (ABR) decision engine for server-side bitrate control.
 *
 * Two-tier architecture:
 *   1. Real-time fallback: immediate threshold-based reactions to packet loss.
 *   2. Event-driven LLM: intelligent per-game target bitrate via LLM AI,
 *      triggered on app switches and network recovery.
 *
 * Clients POST network metrics periodically; the server responds with
 * bitrate adjustments from the fallback layer, while the LLM asynchronously
 * sets the optimal target bitrate ceiling.
 */
#pragma once

#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

namespace abr {

  /// ABR operating mode
  enum class mode_e {
    QUALITY,      ///< Prioritize visual quality
    BALANCED,     ///< Balance quality and latency
    LOW_LATENCY,  ///< Prioritize low latency
  };

  /// ABR configuration per client session
  struct config_t {
    bool enabled = false;
    int min_bitrate_kbps = 2000;
    int max_bitrate_kbps = 150000;
    mode_e mode = mode_e::BALANCED;
  };

  /// Network feedback from client
  struct network_feedback_t {
    double packet_loss;       ///< Packet loss percentage (0-100)
    double rtt_ms;            ///< Round-trip time in ms
    double decode_fps;        ///< Client-side decoded FPS
    int dropped_frames;       ///< Number of dropped frames since last report
    int current_bitrate_kbps; ///< Client's view of current bitrate
  };

  /// Server decision sent back to client
  struct action_t {
    int new_bitrate_kbps = 0;   ///< 0 means no change (immediate action)
    int target_bitrate_kbps = 0; ///< LLM-recommended optimal bitrate (always set when LLM responds)
    std::string reason;
  };

  /// ABR capabilities reported to client
  struct capabilities_t {
    bool supported = true;
    int version = 1;
  };

  /// Recent feedback snapshot for LLM context window
  struct feedback_snapshot_t {
    network_feedback_t feedback;
    std::chrono::steady_clock::time_point timestamp;
  };

  /// Per-client ABR session state
  struct session_state_t {
    config_t config;
    int current_bitrate_kbps = 0;
    int initial_bitrate_kbps = 0;
    std::string app_name;            ///< Game/app from config (may be launcher name)
    std::string foreground_title;     ///< Actual foreground window title (detected at runtime)
    std::string foreground_exe;       ///< Actual foreground process executable name

    /// Rolling window of recent feedback for LLM context (last N seconds)
    std::deque<feedback_snapshot_t> recent_feedback;
    static constexpr size_t MAX_FEEDBACK_HISTORY = 10;

    /// LLM target bitrate: the ideal bitrate recommended by LLM for current app/conditions.
    /// Fallback probe-up will move toward this target. 0 = no recommendation yet.
    int llm_target_bitrate_kbps = 0;

    /// Rate limiting: minimum interval between LLM calls
    std::chrono::steady_clock::time_point last_llm_call;
    static constexpr int LLM_MIN_INTERVAL_SECONDS = 10;

    /// Foreground window detection rate limiting
    std::chrono::steady_clock::time_point last_fg_detect;
    static constexpr int FG_DETECT_INTERVAL_SECONDS = 10;
    uint32_t last_fg_pid = 0;  ///< Cached PID to detect window changes

    /// Fallback: simple threshold-based decisions (always active)
    int consecutive_high_loss = 0;
    int stable_ticks = 0;
    static constexpr int FALLBACK_INTERVAL_SECONDS = 3;
    std::chrono::steady_clock::time_point last_fallback_time;

    /// LLM trigger flags
    bool app_changed = false;         ///< Foreground app changed since last LLM call
    bool network_recovered = false;   ///< Network recovered from turbulence

    /// Async LLM state
    bool llm_in_flight = false;  ///< Prevents concurrent LLM calls (protected by sessions_mutex)
    uint64_t generation = 0;     ///< Monotonic counter to detect stale worker results

    std::chrono::steady_clock::time_point created_time;
  };

  /**
   * @brief Enable ABR for a client session.
   * @param client_name Client identifier.
   * @param cfg ABR configuration.
   * @param initial_bitrate_kbps The bitrate at stream start.
   * @param app_name The game/application currently running.
   */
  void
  enable(const std::string &client_name, const config_t &cfg, int initial_bitrate_kbps, const std::string &app_name);

  /**
   * @brief Disable ABR for a client session.
   */
  void
  disable(const std::string &client_name);

  /**
   * @brief Check if ABR is enabled for a client.
   */
  bool
  is_enabled(const std::string &client_name);

  /**
   * @brief Process network feedback and produce a bitrate action.
   *
   * Runs fallback logic (threshold-based) on every call for real-time response.
   * Triggers LLM asynchronously on app change or network recovery events.
   *
   * @param client_name Client identifier.
   * @param feedback Network metrics from the client.
   * @return Bitrate adjustment action (new_bitrate_kbps == 0 means no change).
   */
  action_t
  process_feedback(const std::string &client_name, const network_feedback_t &feedback);

  /**
   * @brief Get ABR capabilities for capability negotiation.
   */
  capabilities_t
  get_capabilities();

  /**
   * @brief Remove all ABR state for a client (call on session end).
   */
  void
  cleanup(const std::string &client_name);

}  // namespace abr
