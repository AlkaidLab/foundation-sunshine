/**
 * @file src/perf_recorder.cpp
 * @brief Runtime performance snapshot recorder.
 */
#include "perf_recorder.h"
#include "file_handler.h"
#include "logging.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace perf {
  namespace {
    using clock_t = std::chrono::steady_clock;

    constexpr std::size_t SAMPLE_WINDOW_SIZE = 240;
    constexpr std::size_t MAX_RETAINED_COMPLETED_SESSIONS = 8;
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
      double lifetime_sum = 0.0;
      double lifetime_max = 0.0;
    };

    struct probe_bucket_t {
      std::int64_t second = 0;
      std::int64_t first_frame = 0;
      std::int64_t last_frame = 0;
      std::uint64_t sent_frames = 0;
      std::uint64_t wire_bytes = 0;
      std::optional<double> queue_peak;
      std::optional<double> capture_pending_peak;
      std::optional<double> queue_wait_peak_ms;
      std::optional<double> gpu_wait_peak_ms;
      std::optional<double> send_peak_ms;
    };

    struct probe_state_t {
      latency_window_t gpu_wait;
      latency_window_t packetize;
      latency_window_t queue_wait;
      latency_window_t send;
      latency_window_t pacing;
      latency_window_t queue_depth;
      latency_window_t capture_pending_depth;
      std::uint64_t wire_bytes = 0;
      std::uint64_t shared_queue_discards = 0;
      std::uint64_t capture_mailbox_overwrites = 0;
      std::map<std::int64_t, probe_bucket_t> timeline;
      std::uint64_t omitted_seconds = 0;
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
      struct {
        bool enabled = false;
        std::uint64_t frames = 0;
        std::uint64_t encode_failures = 0;
        std::uint64_t packetization_failures = 0;
        std::uint64_t recovery_failures = 0;
      } pyrowave;
      std::unique_ptr<probe_state_t> probe;
    };

    std::mutex recorder_mutex;
    std::unordered_map<std::uint32_t, session_state_t> sessions;
    std::uint32_t latest_session_id = 0;

    session_state_t
    copy_for_snapshot(const session_state_t &source) {
      session_state_t copy;
      copy.metadata = source.metadata;
      copy.active = source.active;
      copy.started_at = source.started_at;
      copy.ended_at = source.ended_at;
      copy.host_latency = source.host_latency;
      copy.pipeline = source.pipeline;
      copy.pyrowave = source.pyrowave;
      if (source.probe) {
        copy.probe = std::make_unique<probe_state_t>(*source.probe);
      }
      return copy;
    }

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
      window.lifetime_sum += latency_ms;
      window.lifetime_max = std::max(window.lifetime_max, latency_ms);
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

    nlohmann::json make_latency_json(const latency_window_t &window, clock_t::time_point now, const std::string &unit = "ms") {
      nlohmann::json result;
      result["samples"] = window.count;
      result["total_samples"] = window.total_samples;
      result["frames_total"] = window.total_frames;
      result["recent_fps"] = round_ms(recent_fps_for_snapshot(window, now));
      result["lifetime_avg_" + unit] = window.total_samples != 0
        ? nlohmann::json(round_ms(window.lifetime_sum / window.total_samples)) : nlohmann::json(nullptr);
      result["lifetime_max_" + unit] = window.total_samples != 0
        ? nlohmann::json(round_ms(window.lifetime_max)) : nlohmann::json(nullptr);

      if (window.count == 0) {
        result["avg_" + unit] = nullptr;
        result["min_" + unit] = nullptr;
        result["max_" + unit] = nullptr;
        result["p95_" + unit] = nullptr;
        result["last_" + unit] = nullptr;
        result["last_sample_age_ms"] = nullptr;
        result["series_" + unit] = nlohmann::json::array();
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

      result["avg_" + unit] = round_ms(sum / ordered.size());
      result["min_" + unit] = round_ms(sorted.front());
      result["max_" + unit] = round_ms(sorted.back());
      result["p95_" + unit] = round_ms(sorted[p95_index]);
      result["last_" + unit] = round_ms(ordered.back());
      result["last_sample_age_ms"] = last_age;
      result["series_" + unit] = nlohmann::json::array();
      for (auto sample : ordered) {
        result["series_" + unit].push_back(round_ms(sample));
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
      item["pyrowave"] = {
        { "enabled", session.pyrowave.enabled },
        { "frames", session.pyrowave.frames },
        { "encode_failures", session.pyrowave.encode_failures },
        { "packetization_failures", session.pyrowave.packetization_failures },
        { "recovery_failures", session.pyrowave.recovery_failures },
      };
      if (session.probe) {
        const auto &probe = *session.probe;
        auto timeline = nlohmann::json::array();
        const auto observation = [](const std::optional<double> &value) {
          return value ? nlohmann::json(round_ms(*value)) : nlohmann::json(nullptr);
        };
        for (const auto &[second, bucket] : probe.timeline) {
          timeline.push_back({
            { "second", bucket.second }, { "first_frame", bucket.first_frame },
            { "last_frame", bucket.last_frame }, { "sent_frames", bucket.sent_frames },
            { "wire_bytes", bucket.wire_bytes }, { "send_queue_peak_frames", observation(bucket.queue_peak) },
            { "capture_pending_peak_frames", observation(bucket.capture_pending_peak) },
            { "send_queue_wait_peak_ms", observation(bucket.queue_wait_peak_ms) },
            { "gpu_completion_wait_peak_ms", observation(bucket.gpu_wait_peak_ms) },
            { "send_dispatch_peak_ms", observation(bucket.send_peak_ms) },
          });
        }
        item["pyrowave_probe"] = {
          { "schema", 1 }, { "clock", "local_monotonic" },
          { "codec", "pyrowave" },
          { "metadata_scope", "latest_session_configuration_not_per_frame" },
          { "percentile_scope", "last_240_samples" },
          { "send_queue_scope", "shared_video_broadcast_queue_all_sessions" },
          { "capture_pending_scope", "single_slot_latest_capture_or_replay_event_per_session_after_take" },
          { "gpu_completion_wait", make_latency_json(probe.gpu_wait, now) },
          { "packetize", make_latency_json(probe.packetize, now) },
          { "send_queue_wait", make_latency_json(probe.queue_wait, now) },
          { "send_dispatch", make_latency_json(probe.send, now) },
          { "pacing", make_latency_json(probe.pacing, now) },
          { "send_queue_depth", make_latency_json(probe.queue_depth, now, "frames") },
          { "capture_pending_depth", make_latency_json(probe.capture_pending_depth, now, "frames") },
          { "wire_bytes", probe.wire_bytes },
          { "wire_bytes_scope", "locally_dispatched_UDP_payload_excluding_IP_UDP_headers" },
          { "gpu_completion_wait_scope", "CPU_fence_wait_including_conversion_encode_readback_not_GPU_execution_time" },
          { "shared_queue_discards_total", probe.shared_queue_discards },
          { "capture_mailbox_overwrites_total", probe.capture_mailbox_overwrites },
          { "timeline", std::move(timeline) }, { "omitted_seconds", probe.omitted_seconds },
        };
      }
      return item;
    }

    void prune_completed_sessions() {
      while (std::count_if(sessions.begin(), sessions.end(), [](const auto &entry) {
        return !entry.second.active;
      }) > MAX_RETAINED_COMPLETED_SESSIONS) {
        auto oldest = sessions.end();
        for (auto it = sessions.begin(); it != sessions.end(); ++it) {
          if (it->second.active || it->first == latest_session_id) {
            continue;
          }
          if (oldest == sessions.end() || it->second.ended_at < oldest->second.ended_at) {
            oldest = it;
          }
        }
        if (oldest == sessions.end()) {
          break;
        }
        sessions.erase(oldest);
      }
    }
  }  // namespace

  void begin_session(const session_metadata_t &metadata) {
    std::lock_guard lock { recorder_mutex };

    auto &session = sessions[metadata.session_id];
    session = {};
    session.metadata = metadata;
    session.active = true;
    session.started_at = clock_t::now();
    if (metadata.probe_enabled) {
      session.probe = std::make_unique<probe_state_t>();
      BOOST_LOG(error) << "PyroWave performance probe enabled; diagnostic collection may affect timing";
    }
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
    std::optional<session_state_t> snapshot;
    {
      std::lock_guard lock { recorder_mutex };
      auto it = sessions.find(session_id);
      if (it == sessions.end() || !it->second.active) return;
      it->second.active = false;
      it->second.ended_at = clock_t::now();
      if (it->second.probe && !it->second.metadata.probe_directory.empty()) {
        try {
          snapshot.emplace(copy_for_snapshot(it->second));
        }
        catch (const std::exception &) {
          BOOST_LOG(warning) << "Unable to prepare PyroWave performance probe";
        }
      }
      prune_completed_sessions();
    }
    if (snapshot) {
      try {
        auto report = make_session_json(*snapshot, snapshot->ended_at);
        report.erase("client_name");
        report["role"] = "sunshine";
        const auto path = file_handler::path_from_utf8(snapshot->metadata.probe_directory);
        std::filesystem::create_directories(path);
        const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count();
        const auto filename = path / ("sunshine-pyrowave-" + std::to_string(stamp) + "-" + std::to_string(session_id) + ".json");
        const auto filename_utf8 = file_handler::path_to_utf8(filename);
        if (file_handler::write_file(filename_utf8.c_str(), report.dump(2)) == 0) {
          BOOST_LOG(info) << "PyroWave performance probe saved: " << filename_utf8;
        }
        else {
          BOOST_LOG(warning) << "Unable to save PyroWave performance probe";
        }
      }
      catch (const std::exception &) {
        BOOST_LOG(warning) << "Unable to save PyroWave performance probe";
      }
    }
  }

  void record_pyrowave_probe(std::uint32_t session_id, const pyrowave_probe_sample_t &sample, clock_t::time_point now) {
    std::lock_guard lock { recorder_mutex };
    auto it = sessions.find(session_id);
    if (it == sessions.end() || !it->second.active || !it->second.probe || sample.frame_id <= 0) return;
    auto &session = it->second;
    auto &probe = *session.probe;
    record_optional_window_sample(probe.gpu_wait, sample.gpu_completion_wait_ms, now);
    record_optional_window_sample(probe.packetize, sample.packetize_ms, now);
    record_optional_window_sample(probe.queue_wait, sample.send_queue_wait_ms, now);
    record_optional_window_sample(probe.send, sample.send_dispatch_ms, now);
    record_optional_window_sample(probe.pacing, sample.pacing_ms, now);
    record_optional_window_sample(probe.queue_depth, sample.send_queue_frames, now);
    record_optional_window_sample(probe.capture_pending_depth, sample.capture_pending_frames, now);
    probe.wire_bytes += sample.wire_bytes;
    probe.shared_queue_discards = std::max(probe.shared_queue_discards, sample.shared_queue_discards);
    probe.capture_mailbox_overwrites = std::max(probe.capture_mailbox_overwrites, sample.capture_mailbox_overwrites);
    const auto second = std::max<std::int64_t>(0,
      std::chrono::duration_cast<std::chrono::seconds>(now - session.started_at).count());
    // Encoder and sender samples can acquire the recorder lock in a different
    // order than their timestamps. Keep one chronological bucket per second.
    if (probe.timeline.size() == 600 && second < probe.timeline.begin()->first) return;
    auto bucket_it = probe.timeline.try_emplace(second,
      probe_bucket_t { .second = second, .first_frame = sample.frame_id, .last_frame = sample.frame_id }).first;
    if (probe.timeline.size() > 600) {
      probe.timeline.erase(probe.timeline.begin());
      ++probe.omitted_seconds;
    }
    auto &bucket = bucket_it->second;
    bucket.first_frame = std::min(bucket.first_frame, sample.frame_id);
    bucket.last_frame = std::max(bucket.last_frame, sample.frame_id);
    bucket.sent_frames += sample.send_dispatch_ms.has_value();
    bucket.wire_bytes += sample.wire_bytes;
    const auto peak = [](std::optional<double> &target, const std::optional<double> &value) {
      if (value && std::isfinite(*value) && *value >= 0.0) target = std::max(target.value_or(0.0), *value);
    };
    peak(bucket.queue_peak, sample.send_queue_frames);
    peak(bucket.capture_pending_peak, sample.capture_pending_frames);
    peak(bucket.queue_wait_peak_ms, sample.send_queue_wait_ms);
    peak(bucket.gpu_wait_peak_ms, sample.gpu_completion_wait_ms);
    peak(bucket.send_peak_ms, sample.send_dispatch_ms);
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
    if (sample.pyrowave) {
      it->second.pyrowave.enabled = true;
      ++it->second.pyrowave.frames;
    }
  }

  void record_pyrowave_failure(std::uint32_t session_id, pyrowave_failure_stage_e stage) {
    std::lock_guard lock { recorder_mutex };

    auto it = sessions.find(session_id);
    if (it == sessions.end()) {
      return;
    }

    it->second.pyrowave.enabled = true;
    switch (stage) {
      case pyrowave_failure_stage_e::encode:
        ++it->second.pyrowave.encode_failures;
        break;
      case pyrowave_failure_stage_e::packetize:
        ++it->second.pyrowave.packetization_failures;
        break;
      case pyrowave_failure_stage_e::recovery:
        ++it->second.pyrowave.recovery_failures;
        break;
    }
  }

  nlohmann::json current_snapshot_json() {
    std::vector<session_state_t> snapshot;
    clock_t::time_point now;
    std::uint32_t snapshot_latest_session_id;
    {
      std::lock_guard lock { recorder_mutex };
      now = clock_t::now();
      snapshot_latest_session_id = latest_session_id;
      snapshot.reserve(sessions.size());
      for (const auto &[session_id, session] : sessions) {
        snapshot.push_back(copy_for_snapshot(session));
      }
    }
    nlohmann::json result;
    result["success"] = true;
    result["status_code"] = 200;
    result["status_message"] = "Success";
    result["sample_window"] = SAMPLE_WINDOW_SIZE;
    result["sessions"] = nlohmann::json::array();
    result["active_sessions"] = 0;
    result["latest_session_id"] = snapshot_latest_session_id;

    for (const auto &session : snapshot) {
      if (session.active) {
        result["active_sessions"] = result["active_sessions"].get<int>() + 1;
      }
      result["sessions"].push_back(make_session_json(session, now));
    }

    return result;
  }

}  // namespace perf
