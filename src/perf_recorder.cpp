/**
 * @file src/perf_recorder.cpp
 * @brief Runtime performance snapshot recorder.
 */
#include "perf_recorder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace perf {
  namespace {
    using clock_t = std::chrono::steady_clock;

    constexpr std::size_t SAMPLE_WINDOW_SIZE = 240;
    constexpr auto RECENT_FPS_STALE_AFTER = std::chrono::seconds(2);

    struct latency_window_t {
      std::array<double, SAMPLE_WINDOW_SIZE> samples {};
      std::size_t next_index = 0;
      std::size_t count = 0;
      std::uint64_t total_samples = 0;
      std::uint64_t total_frames = 0;
      clock_t::time_point last_sample_at {};
      clock_t::time_point fps_bucket_started_at {};
      std::uint32_t fps_bucket_frames = 0;
      double recent_fps = 0.0;
    };

    struct pipeline_windows_t {
      latency_window_t capture_to_convert;
      latency_window_t convert;
      latency_window_t encode_queue;
      latency_window_t encode;
      latency_window_t packet_to_broadcast;
      latency_window_t total;
    };

    struct session_state_t {
      session_metadata_t metadata;
      bool active = true;
      clock_t::time_point started_at = clock_t::now();
      clock_t::time_point ended_at {};
      latency_window_t host_latency;
      pipeline_windows_t pipeline;
    };

    std::mutex recorder_mutex;
    std::unordered_map<std::uint32_t, session_state_t> sessions;
    std::uint32_t latest_session_id = 0;

    double round_ms(double value) {
      return std::round(value * 100.0) / 100.0;
    }

    double recent_fps_for_snapshot(const latency_window_t &window, clock_t::time_point now) {
      if (window.count == 0 || window.last_sample_at == clock_t::time_point {}) {
        return 0.0;
      }

      if (now - window.last_sample_at > RECENT_FPS_STALE_AFTER) {
        return 0.0;
      }

      return window.recent_fps;
    }

    void record_window_sample(latency_window_t &window, double latency_ms, clock_t::time_point now, bool count_frame) {
      if (!std::isfinite(latency_ms) || latency_ms < 0.0) {
        return;
      }

      window.samples[window.next_index] = latency_ms;
      window.next_index = (window.next_index + 1) % SAMPLE_WINDOW_SIZE;
      window.count = std::min<std::size_t>(window.count + 1, SAMPLE_WINDOW_SIZE);
      ++window.total_samples;
      if (count_frame) {
        ++window.total_frames;
      }
      window.last_sample_at = now;

      if (window.fps_bucket_started_at == clock_t::time_point {}) {
        window.fps_bucket_started_at = now;
      }

      if (count_frame) {
        ++window.fps_bucket_frames;
        const auto bucket_elapsed = now - window.fps_bucket_started_at;
        if (bucket_elapsed >= std::chrono::seconds(1)) {
          const auto bucket_seconds = std::chrono::duration<double>(bucket_elapsed).count();
          window.recent_fps = window.fps_bucket_frames / bucket_seconds;
          window.fps_bucket_started_at = now;
          window.fps_bucket_frames = 0;
        }
      }
    }

    void record_optional_window_sample(latency_window_t &window, const std::optional<double> &latency_ms, clock_t::time_point now) {
      if (!latency_ms) {
        return;
      }

      record_window_sample(window, *latency_ms, now, false);
    }

    nlohmann::json make_latency_json(const latency_window_t &window, clock_t::time_point now) {
      nlohmann::json result;
      result["samples"] = window.count;
      result["total_samples"] = window.total_samples;
      result["frames_total"] = window.total_frames;
      result["recent_fps"] = round_ms(recent_fps_for_snapshot(window, now));

      if (window.count == 0) {
        result["avg_ms"] = nullptr;
        result["min_ms"] = nullptr;
        result["max_ms"] = nullptr;
        result["p95_ms"] = nullptr;
        result["last_ms"] = nullptr;
        result["last_sample_age_ms"] = nullptr;
        result["series_ms"] = nlohmann::json::array();
        return result;
      }

      std::vector<double> ordered;
      ordered.reserve(window.count);
      for (std::size_t i = 0; i < window.count; ++i) {
        const auto physical_index = (window.next_index + SAMPLE_WINDOW_SIZE - window.count + i) % SAMPLE_WINDOW_SIZE;
        ordered.push_back(window.samples[physical_index]);
      }

      double sum = 0.0;
      for (auto sample : ordered) {
        sum += sample;
      }

      auto sorted = ordered;
      std::sort(sorted.begin(), sorted.end());
      const auto p95_index = static_cast<std::size_t>(std::ceil(sorted.size() * 0.95)) - 1;

      const auto last_age = std::chrono::duration_cast<std::chrono::milliseconds>(now - window.last_sample_at).count();

      result["avg_ms"] = round_ms(sum / ordered.size());
      result["min_ms"] = round_ms(sorted.front());
      result["max_ms"] = round_ms(sorted.back());
      result["p95_ms"] = round_ms(sorted[p95_index]);
      result["last_ms"] = round_ms(ordered.back());
      result["last_sample_age_ms"] = last_age;
      result["series_ms"] = nlohmann::json::array();
      for (auto sample : ordered) {
        result["series_ms"].push_back(round_ms(sample));
      }

      return result;
    }

    nlohmann::json make_pipeline_json(const pipeline_windows_t &pipeline, clock_t::time_point now) {
      nlohmann::json result;
      result["capture_to_convert"] = make_latency_json(pipeline.capture_to_convert, now);
      result["convert"] = make_latency_json(pipeline.convert, now);
      result["encode_queue"] = make_latency_json(pipeline.encode_queue, now);
      result["encode"] = make_latency_json(pipeline.encode, now);
      result["packet_to_broadcast"] = make_latency_json(pipeline.packet_to_broadcast, now);
      result["total"] = make_latency_json(pipeline.total, now);
      return result;
    }

    nlohmann::json make_session_json(const session_state_t &session, clock_t::time_point now) {
      nlohmann::json item;
      item["session_id"] = session.metadata.session_id;
      item["client_name"] = session.metadata.client_name;
      item["active"] = session.active;
      item["control_only"] = session.metadata.control_only;
      item["width"] = session.metadata.width;
      item["height"] = session.metadata.height;
      item["fps"] = session.metadata.fps;
      item["bitrate_kbps"] = session.metadata.bitrate_kbps;
      item["encoder"] = session.metadata.encoder;
      item["capture"] = session.metadata.capture;
      item["uptime_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>((session.active ? now : session.ended_at) - session.started_at).count();
      item["host_latency"] = make_latency_json(session.host_latency, now);
      item["pipeline"] = make_pipeline_json(session.pipeline, now);
      return item;
    }
  }  // namespace

  void begin_session(const session_metadata_t &metadata) {
    std::lock_guard lock { recorder_mutex };

    auto &session = sessions[metadata.session_id];
    session = {};
    session.metadata = metadata;
    session.active = true;
    session.started_at = clock_t::now();
    session.host_latency.fps_bucket_started_at = session.started_at;
    session.pipeline.capture_to_convert.fps_bucket_started_at = session.started_at;
    session.pipeline.convert.fps_bucket_started_at = session.started_at;
    session.pipeline.encode_queue.fps_bucket_started_at = session.started_at;
    session.pipeline.encode.fps_bucket_started_at = session.started_at;
    session.pipeline.packet_to_broadcast.fps_bucket_started_at = session.started_at;
    session.pipeline.total.fps_bucket_started_at = session.started_at;
    latest_session_id = metadata.session_id;
  }

  void end_session(std::uint32_t session_id) {
    std::lock_guard lock { recorder_mutex };

    auto it = sessions.find(session_id);
    if (it == sessions.end()) {
      return;
    }

    sessions.erase(it);
    if (latest_session_id == session_id) {
      latest_session_id = 0;
      for (const auto &[candidate_id, session] : sessions) {
        if (session.active && candidate_id > latest_session_id) {
          latest_session_id = candidate_id;
        }
      }
    }
  }

  void update_session_bitrate(std::uint32_t session_id, int bitrate_kbps) {
    std::lock_guard lock { recorder_mutex };

    auto it = sessions.find(session_id);
    if (it == sessions.end()) {
      return;
    }

    it->second.metadata.bitrate_kbps = bitrate_kbps;
  }

  void update_session_display(std::uint32_t session_id, int width, int height, int fps) {
    std::lock_guard lock { recorder_mutex };

    auto it = sessions.find(session_id);
    if (it == sessions.end()) {
      return;
    }

    it->second.metadata.width = width;
    it->second.metadata.height = height;
    it->second.metadata.fps = fps;
  }

  void record_host_latency(std::uint32_t session_id, double latency_ms, std::chrono::steady_clock::time_point now) {
    std::lock_guard lock { recorder_mutex };

    auto it = sessions.find(session_id);
    if (it == sessions.end()) {
      return;
    }

    record_window_sample(it->second.host_latency, latency_ms, now, true);
  }

  void record_pipeline_sample(std::uint32_t session_id, const pipeline_sample_t &sample, std::chrono::steady_clock::time_point now) {
    std::lock_guard lock { recorder_mutex };

    auto it = sessions.find(session_id);
    if (it == sessions.end()) {
      return;
    }

    auto &pipeline = it->second.pipeline;
    record_optional_window_sample(pipeline.capture_to_convert, sample.capture_to_convert_ms, now);
    record_optional_window_sample(pipeline.convert, sample.convert_ms, now);
    record_optional_window_sample(pipeline.encode_queue, sample.encode_queue_ms, now);
    record_optional_window_sample(pipeline.encode, sample.encode_ms, now);
    record_optional_window_sample(pipeline.packet_to_broadcast, sample.packet_to_broadcast_ms, now);
    if (sample.total_ms) {
      record_window_sample(pipeline.total, *sample.total_ms, now, true);
    }
  }

  nlohmann::json current_snapshot_json() {
    std::lock_guard lock { recorder_mutex };

    const auto now = clock_t::now();
    nlohmann::json result;
    result["success"] = true;
    result["status_code"] = 200;
    result["status_message"] = "Success";
    result["sample_window"] = SAMPLE_WINDOW_SIZE;
    result["sessions"] = nlohmann::json::array();
    result["active_sessions"] = 0;
    result["latest_session_id"] = latest_session_id;

    for (const auto &[session_id, session] : sessions) {
      if (session.active) {
        result["active_sessions"] = result["active_sessions"].get<int>() + 1;
      }
      result["sessions"].push_back(make_session_json(session, now));
    }

    return result;
  }

}  // namespace perf
