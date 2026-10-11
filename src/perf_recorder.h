/**
 * @file src/perf_recorder.h
 * @brief Lightweight runtime performance snapshots for active streaming sessions.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

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
    bool probe_enabled = false;
    std::string probe_directory;
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

  struct pyrowave_probe_sample_t {
    std::int64_t frame_id = 0;
    std::optional<double> gpu_completion_wait_ms;
    std::optional<double> packetize_ms;
    std::optional<double> send_queue_wait_ms;
    std::optional<double> send_dispatch_ms;
    std::optional<double> pacing_ms;
    std::optional<double> send_queue_frames;
    std::optional<double> capture_pending_frames;
    std::uint64_t capture_mailbox_overwrites = 0;
    std::uint64_t shared_queue_discards = 0;
    std::uint64_t wire_bytes = 0;
  };

  void begin_session(const session_metadata_t &metadata);
  void end_session(std::uint32_t session_id);
  void update_session_bitrate(std::uint32_t session_id, int bitrate_kbps);
  void update_session_display(std::uint32_t session_id, int width, int height, int fps);
  void record_host_latency(std::uint32_t session_id, double latency_ms, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
  void record_pipeline_sample(std::uint32_t session_id, const pipeline_sample_t &sample, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
  void record_pyrowave_failure(std::uint32_t session_id, pyrowave_failure_stage_e stage);
  void record_pyrowave_probe(std::uint32_t session_id, const pyrowave_probe_sample_t &sample,
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

  nlohmann::json current_snapshot_json();

}  // namespace perf
