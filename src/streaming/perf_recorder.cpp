/**
 * @file src/streaming/perf_recorder.cpp
 * @brief Runtime performance snapshot recorder.
 */
#include "perf_recorder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

namespace perf {
  std::atomic<std::uint64_t> frame_rate_collection_generation { 0 };
  namespace detail {
    std::atomic<std::uint32_t> source_updates_in_flight { 0 };
    std::atomic<bool> source_drain_requested { false };
  }

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
      std::shared_ptr<capture_source_counter_t> capture_source;
      std::uint64_t source_baseline = 0;
      std::uint64_t frame_rate_generation = 0;
      frame_rate_window_t::counts_t frame_counts {};
      frame_rate_window_t frame_rates;
      struct {
        bool enabled = false;
        std::uint64_t frames = 0;
        std::uint64_t encode_failures = 0;
        std::uint64_t packetization_failures = 0;
        std::uint64_t recovery_failures = 0;
      } pyrowave;
    };

    std::mutex recorder_mutex;
    std::mutex source_drain_mutex;
    std::condition_variable source_updates_finished;
    std::unordered_map<std::uint32_t, session_state_t> sessions;
    std::uint32_t latest_session_id = 0;
    std::unordered_map<std::string, clock_t::time_point> frame_rate_subscriptions;
    std::condition_variable subscription_changed;
    std::uint64_t next_collection_generation = 0;

    void refresh_source_total(session_state_t &session) {
      if (session.capture_source) {
        const auto frames = session.capture_source->frames.load(std::memory_order_relaxed);
        session.frame_counts[0] = frames >= session.source_baseline ? frames - session.source_baseline : 0;
      }
    }

    // Called with recorder_mutex held; frame callbacks never manage subscriptions.
    void update_collection_state(clock_t::time_point now) {
      const bool enabled = !frame_rate_subscriptions.empty();
      if (enabled == (frame_rate_collection_epoch() != 0)) return;
      if (!enabled) {
        detail::source_drain_requested.store(true, std::memory_order_seq_cst);
        frame_rate_collection_generation.store(0, std::memory_order_seq_cst);
        // Source hooks never take recorder_mutex, so they can complete while
        // the transition holds it. No new epoch can begin until this drain ends.
        {
          std::unique_lock lock { source_drain_mutex };
          source_updates_finished.wait(lock, []() {
            return detail::source_updates_in_flight.load(std::memory_order_seq_cst) == 0;
          });
        }
        detail::source_drain_requested.store(false, std::memory_order_seq_cst);
        for (auto &[id, session] : sessions) {
          if (session.active) refresh_source_total(session);
        }
        return;
      }
      const auto generation = ++next_collection_generation;
      for (auto &[id, session] : sessions) {
        if (!session.active) continue;
        session.frame_counts = {};
        session.source_baseline = session.capture_source ? session.capture_source->frames.load(std::memory_order_relaxed) : 0;
        session.frame_rate_generation = generation;
        session.frame_rates.reset(now, session.frame_counts);
      }
      frame_rate_collection_generation.store(generation, std::memory_order_release);
    }

    void expire_subscriptions(clock_t::time_point now) {
      std::erase_if(frame_rate_subscriptions, [now](const auto &entry) { return entry.second <= now; });
      update_collection_state(now);
    }

    class subscription_expiry_worker_t {
    public:
      subscription_expiry_worker_t(): worker([this]() {
        std::unique_lock lock { recorder_mutex };
        while (!stopping) {
          expire_subscriptions(clock_t::now());
          if (frame_rate_subscriptions.empty()) {
            subscription_changed.wait(lock, [&]() { return stopping || !frame_rate_subscriptions.empty(); });
          }
          else {
            const auto earliest = std::min_element(frame_rate_subscriptions.begin(), frame_rate_subscriptions.end(),
              [](const auto &left, const auto &right) { return left.second < right.second; });
            const auto deadline = earliest->second;
            subscription_changed.wait_until(lock, deadline);
          }
        }
      }) {}

      ~subscription_expiry_worker_t() {
        {
          std::lock_guard lock { recorder_mutex };
          stopping = true;
          subscription_changed.notify_all();
        }
        worker.join();
      }

    private:
      bool stopping = false;
      std::thread worker;
    };

    void start_subscription_expiry_worker() {
      // Owned shutdown completes before recorder globals are destroyed, without
      // relying on a stop-token/atomic-wait pool with a later static lifetime.
      static subscription_expiry_worker_t worker;
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

    nlohmann::json make_frame_rates_json(session_state_t &session, clock_t::time_point now) {
      const auto generation = frame_rate_collection_epoch();
      const bool enabled = generation && session.frame_rate_generation == generation;
      auto kind = capture_source_kind_e::unavailable;
      if (session.capture_source) {
        kind = session.capture_source->kind.load(std::memory_order_relaxed);
        if (session.active && enabled) {
          refresh_source_total(session);
        }
      }
      const char *source_kind = "unavailable";
      switch (kind) {
        case capture_source_kind_e::dxgi_updates:
          source_kind = "dxgi_updates";
          break;
        case capture_source_kind_e::wgc_arrivals:
          source_kind = "wgc_arrivals";
          break;
        case capture_source_kind_e::vdd_published:
          source_kind = "vdd_published";
          break;
        default:
          break;
      }
      const auto rates = enabled ? session.frame_rates.sample(now, session.frame_counts) : std::nullopt;
      nlohmann::json result = {
        { "enabled", enabled },
        { "source_kind", source_kind },
        { "ready", rates.has_value() },
        { "window_ms", enabled ? round_ms(session.frame_rates.duration_seconds() * 1000.0) : 0.0 },
      };
      constexpr std::array keys { "source", "capture", "submitted", "encoded", "repeated" };
      for (std::size_t i = 0; i < keys.size(); ++i) {
        auto &stage = result[keys[i]];
        stage["frames_total"] = session.frame_counts[i];
        if (!enabled || (i == 0 && kind == capture_source_kind_e::unavailable)) {
          stage["fps"] = nullptr;
        }
        else if (!session.active) {
          stage["fps"] = 0.0;
        }
        else if (rates) {
          stage["fps"] = round_ms((*rates)[i]);
        }
        else {
          stage["fps"] = nullptr;
        }
      }
      return result;
    }

    nlohmann::json make_session_json(session_state_t &session, clock_t::time_point now) {
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
      item["frame_rates"] = make_frame_rates_json(session, now);
      item["pyrowave"] = {
        { "enabled", session.pyrowave.enabled },
        { "frames", session.pyrowave.frames },
        { "encode_failures", session.pyrowave.encode_failures },
        { "packetization_failures", session.pyrowave.packetization_failures },
        { "recovery_failures", session.pyrowave.recovery_failures },
      };
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

  namespace detail {
    void notify_source_drain() {
      // Pair notification with the waiter's predicate check to avoid lost wakes.
      std::lock_guard lock { source_drain_mutex };
      source_updates_finished.notify_all();
    }
  }

  void begin_session(const session_metadata_t &metadata) {
    std::lock_guard lock { recorder_mutex };

    auto &session = sessions[metadata.session_id];
    session = {};
    session.metadata = metadata;
    session.active = true;
    session.frame_rate_generation = frame_rate_collection_epoch();
    session.started_at = clock_t::now();
    session.frame_rates.reset(session.started_at, session.frame_counts);
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

    if (it->second.active) {
      if (frame_rate_collection_epoch()) {
        refresh_source_total(it->second);
      }
      it->second.active = false;
      it->second.ended_at = clock_t::now();
      prune_completed_sessions();
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

  void record_capture_frame(std::uint32_t session_id, const std::shared_ptr<capture_source_counter_t> &source, clock_t::time_point now) {
    const auto generation = frame_rate_collection_epoch();
    if (!generation) return;
    std::lock_guard lock { recorder_mutex };
    if (frame_rate_collection_epoch() != generation) return;
    const auto it = sessions.find(session_id);
    if (it == sessions.end() || !it->second.active) return;
    auto &session = it->second;
    if (now == clock_t::time_point {}) now = clock_t::now();
    if (session.capture_source != source) {
      session.capture_source = source;
      session.source_baseline = source ? source->frames.load(std::memory_order_relaxed) : 0;
      session.frame_counts[0] = 0;
      // A new display/channel needs a fresh baseline, including on reinit.
      session.frame_rates.reset(now, session.frame_counts);
    }
    ++session.frame_counts[1];
  }

  void record_encode_submission(std::uint32_t session_id, bool repeat) {
    const auto generation = frame_rate_collection_epoch();
    if (!generation) return;
    std::lock_guard lock { recorder_mutex };
    if (frame_rate_collection_epoch() != generation) return;
    const auto it = sessions.find(session_id);
    if (it == sessions.end() || !it->second.active) return;
    ++it->second.frame_counts[2];
    if (repeat) ++it->second.frame_counts[4];
  }

  void record_encoded_frame(std::uint32_t session_id) {
    const auto generation = frame_rate_collection_epoch();
    if (!generation) return;
    std::lock_guard lock { recorder_mutex };
    if (frame_rate_collection_epoch() != generation) return;
    const auto it = sessions.find(session_id);
    if (it != sessions.end() && it->second.active) ++it->second.frame_counts[3];
  }

  nlohmann::json set_frame_rate_subscription(const std::string &consumer_id, bool enabled, std::chrono::milliseconds lease) {
    const auto valid_character = [](unsigned char ch) {
      return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
             ch == '-' || ch == '_' || ch == '.' || ch == ':';
    };
    if (consumer_id.empty() || consumer_id.size() > 128 || !std::all_of(consumer_id.begin(), consumer_id.end(), valid_character)) {
      throw std::invalid_argument("consumer_id must contain 1-128 ASCII identifier characters");
    }
    if (lease < std::chrono::seconds(1) || lease > std::chrono::seconds(30)) {
      throw std::invalid_argument("lease_ms must be between 1000 and 30000");
    }
    if (enabled) start_subscription_expiry_worker();
    std::lock_guard lock { recorder_mutex };
    const auto now = clock_t::now();
    expire_subscriptions(now);
    if (enabled) {
      if (!frame_rate_subscriptions.contains(consumer_id) && frame_rate_subscriptions.size() >= 64) {
        throw std::invalid_argument("too many frame-rate subscribers");
      }
      frame_rate_subscriptions[consumer_id] = now + lease;
    }
    else {
      frame_rate_subscriptions.erase(consumer_id);
    }
    update_collection_state(now);
    subscription_changed.notify_all();
    return { { "enabled", frame_rate_collection_epoch() != 0 }, { "subscribers", frame_rate_subscriptions.size() }, { "lease_ms", lease.count() } };
  }

  nlohmann::json current_snapshot_json(clock_t::time_point now) {
    std::lock_guard lock { recorder_mutex };

    nlohmann::json result;
    result["success"] = true;
    result["status_code"] = 200;
    result["status_message"] = "Success";
    result["sample_window"] = SAMPLE_WINDOW_SIZE;
    result["sessions"] = nlohmann::json::array();
    result["active_sessions"] = 0;
    result["latest_session_id"] = latest_session_id;
    result["frame_rate_collection"] = {
      { "enabled", frame_rate_collection_epoch() != 0 },
      { "subscribers", frame_rate_subscriptions.size() },
    };

    for (auto &[session_id, session] : sessions) {
      if (session.active) {
        result["active_sessions"] = result["active_sessions"].get<int>() + 1;
      }
      result["sessions"].push_back(make_session_json(session, now));
    }

    return result;
  }

}  // namespace perf
