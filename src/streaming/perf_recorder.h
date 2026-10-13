/**
 * @file src/streaming/perf_recorder.h
 * @brief Lightweight runtime performance snapshots for active streaming sessions.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "frame_rate_tracker.h"

namespace perf {

  struct session_metadata_t {
    std::uint32_t session_id;
    std::string client_name;
    int width;
    int height;
    int fps;
    int bitrate_kbps;
    std::string encoder;
    std::string capture;
    bool control_only;
  };

  struct pipeline_sample_t {
    std::optional<double> capture_to_convert_ms;
    std::optional<double> convert_ms;
    std::optional<double> encode_queue_ms;
    std::optional<double> encode_ms;
    std::optional<double> packet_to_broadcast_ms;
    std::optional<double> total_ms;
    bool pyrowave = false;
  };

  enum class pyrowave_failure_stage_e {
    encode,
    packetize,
    recovery,
  };

  void begin_session(const session_metadata_t &metadata);
  void end_session(std::uint32_t session_id);
  void update_session_bitrate(std::uint32_t session_id, int bitrate_kbps);
  void update_session_display(std::uint32_t session_id, int width, int height, int fps);
  void record_host_latency(std::uint32_t session_id, double latency_ms, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
  void record_pipeline_sample(std::uint32_t session_id, const pipeline_sample_t &sample, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
  void record_pyrowave_failure(std::uint32_t session_id, pyrowave_failure_stage_e stage);

  void record_capture_frame(std::uint32_t session_id, const std::shared_ptr<capture_source_counter_t> &source,
    std::chrono::steady_clock::time_point now = {});
  void record_encode_submission(std::uint32_t session_id, bool repeat);
  void record_encoded_frame(std::uint32_t session_id);

  /**
   * @brief Acquire/renew or release a consumer's frame-rate collection lease.
   * Collection stops after the last lease is released or expires. Re-enabling
   * resets active session windows; renewing an existing lease does not reset them.
   * @param consumer_id Caller-owned identifier, 1-128 ASCII identifier characters.
   * @param enabled Whether to acquire/renew (true) or release (false) the lease.
   * @param lease Lease lifetime, between 1000 and 30000 milliseconds.
   * @return Current collection state, subscriber count and granted lease lifetime.
   * @throws std::invalid_argument For invalid identifiers, lifetime or capacity.
   */
  nlohmann::json set_frame_rate_subscription(const std::string &consumer_id, bool enabled,
    std::chrono::milliseconds lease = std::chrono::seconds(30));

  nlohmann::json current_snapshot_json(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

}  // namespace perf
