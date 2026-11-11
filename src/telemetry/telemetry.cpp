/**
 * @file src/telemetry/telemetry.cpp
 * @brief Telemetry v1 in the host: the sender thread, the HTTP transport, the log tally and the glue to the config.
 */
#include "telemetry.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <boost/log/core.hpp>
#include <boost/log/sinks/basic_sink_backend.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/log/attributes/value_extraction.hpp>

#include "service.h"
#include "transport.h"
#include "src/build_info.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/system_info.h"
#include "version.h"

using json = nlohmann::json;
using namespace std::literals;
namespace bl = boost::log;

namespace telemetry {

  namespace {
    using clock_t = std::chrono::steady_clock;

    // taken when this file is loaded: the closest cheap stand-in for the start of the process
    const clock_t::time_point process_start = clock_t::now();

    constexpr auto FLUSH_INTERVAL = 30s;
    constexpr auto TICK_INTERVAL = 2s;
    constexpr auto CONFIG_RECHECK = 60s;

    // Both choices are on unless the person said off, but the answer is only known once the config file was read: until then (or when it cannot be read) nothing is counted or sent.
    struct consent_cache_t {
      std::atomic<bool> usage { false };
      std::atomic<bool> errors { false };
    } consent_cache;

    std::shared_ptr<service_t> &
    service_slot() {
      static std::shared_ptr<service_t> slot;
      return slot;
    }
    std::mutex slot_mutex;  // guards the slot pointer itself; copies of the shared_ptr are used outside of it

    std::shared_ptr<service_t>
    current() {
      std::lock_guard lock { slot_mutex };
      return service_slot();
    }

    std::mutex probe_mutex;
    std::function<std::vector<session_probe_t>()> probe_fn;

    std::string
    read_endpoint_from(const std::map<std::string, std::string> &snapshot) {
      const auto found = snapshot.find(std::string { CONFIG_KEY_ENDPOINT });
      if (found == snapshot.end() || found->second.empty()) return std::string { DEFAULT_ENDPOINT };
      auto value = found->second;
      while (!value.empty() && value.back() == '/') value.pop_back();
      return value;
    }

    consent_t
    consent_from_snapshot(const std::map<std::string, std::string> &snapshot) {
      const auto usage = snapshot.find(std::string { CONFIG_KEY_USAGE });
      const auto errors = snapshot.find(std::string { CONFIG_KEY_ERRORS });
      return consent_from_config(usage == snapshot.end() ? std::string {} : usage->second, errors == snapshot.end() ? std::string {} : errors->second, usage != snapshot.end(), errors != snapshot.end());
    }

    void
    refresh_consent_cache() {
      if (const auto snapshot = config::get_config_snapshot()) {
        const auto c = consent_from_snapshot(*snapshot);
        consent_cache.usage.store(c.usage, std::memory_order_relaxed);
        consent_cache.errors.store(c.errors, std::memory_order_relaxed);
      }
    }

    post_result_t
    http_post(const std::string &url, const std::string &content_type, const std::string &body) {
      return post_once(url, content_type, body, std::string { "FoundationSunshine/" } + PROJECT_VER);
    }

    std::string
    channel_name() {
      const auto channel = build_info::compiled().channel;
      if (channel == "stable" || channel == "beta" || channel == "dev") return channel;
      return "dev";
    }

    std::string
    arch_name() {
#if defined(__aarch64__) || defined(_M_ARM64)
      return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
      return "x86_64";
#else
      return "other";
#endif
    }

    std::string
    os_name() {
#if defined(_WIN32)
      return "windows";
#elif defined(__APPLE__)
      return "macos";
#else
      return "linux";
#endif
    }

    std::string
    gpu_vendor_of(std::string_view vendor) {
      std::string v { vendor };
      std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (v.find("nvidia") != std::string::npos) return "nvidia";
      if (v.find("amd") != std::string::npos || v.find("advanced micro") != std::string::npos || v.find("ati") != std::string::npos) return "amd";
      if (v.find("intel") != std::string::npos) return "intel";
      if (v.find("apple") != std::string::npos) return "apple";
      return v.empty() ? "" : "other";
    }

    // which part of the host a log line comes from: only a known word at the start of the line names a component
    std::string
    component_of(const char *message, std::size_t length) {
      static const std::vector<std::string_view> known {
        "config", "nvhttp", "rtsp", "stream", "video", "audio", "input", "capture", "display", "encoder", "nvenc", "webhook", "vdd", "sparxie", "upnp", "network", "clipboard", "gamepad", "tray", "process", "doctor", "pair", "nat", "mic",
      };
      std::string head;
      for (std::size_t i = 0; i < length && i < 24; ++i) {
        const auto c = static_cast<unsigned char>(message[i]);
        if (!std::isalnum(c)) break;
        head.push_back(static_cast<char>(std::tolower(c)));
      }
      for (const auto &k : known)
        if (head == k) return head;
      return "host";
    }

    class log_backend_t: public bl::sinks::basic_sink_backend<bl::sinks::synchronized_feeding> {
    public:
      void
      consume(const bl::record_view &record) {
        // runs on the thread that logged: a counter and a copy of the line, nothing else (the line is scrubbed later, on the sender thread)
        try {
          const auto severity = record.attribute_values()["Severity"].extract<int>();
          if (!severity || severity.get() < 3 || severity.get() > 5) return;
          const auto service = current();
          if (!service) return;
          const auto message = record.attribute_values()["Message"].extract<std::string>();
          const std::string text = message ? message.get() : std::string {};
          service->note_log(severity.get(), text.c_str(), text.size(), component_of(text.c_str(), text.size()));
        }
        catch (...) {
        }
      }
    };
    using log_sink_t = bl::sinks::synchronous_sink<log_backend_t>;
    boost::shared_ptr<log_sink_t> log_sink;

    class runner_t {
    public:
      explicit runner_t(std::shared_ptr<service_t> service):
          service_(std::move(service)) {
        service_->set_wake([this] { poke(); });
        thread_ = std::thread([this] { run(); });
      }
      ~runner_t() { stop(); }

      void
      poke() {
        {
          std::lock_guard lock { mutex_ };
          poked_ = true;
        }
        cv_.notify_all();
      }

      /** Stop the thread. A POST in flight can take up to its timeout: the host's shutdown must not wait for it, so after `grace` the thread is let go (it keeps the service alive until it ends). */
      bool
      stop(std::chrono::milliseconds grace = 1500ms) {
        {
          std::lock_guard lock { mutex_ };
          if (stopping_) return true;
          stopping_ = true;
        }
        cv_.notify_all();
        std::unique_lock lock { mutex_ };
        const bool finished = cv_.wait_for(lock, grace, [&] { return done_; });
        lock.unlock();
        if (!thread_.joinable()) return true;
        if (finished) thread_.join();
        else thread_.detach();
        return finished;
      }

    private:
      void
      fill_context() {
        auto ctx = service_->context();
        try {
          const auto snapshot = system_info::sample(config::video.adapter_name, false);
          ctx.os_version = snapshot.os.version + (snapshot.os.build.empty() ? "" : "." + snapshot.os.build);
          if (ctx.os_version.size() > 48) ctx.os_version.resize(48);
          for (const auto &gpu : snapshot.gpus) {
            if (gpu.is_streaming_adapter) {
              ctx.gpu_vendor = gpu_vendor_of(gpu.vendor);
              break;
            }
          }
          if (ctx.gpu_vendor.empty() && !snapshot.gpus.empty()) ctx.gpu_vendor = gpu_vendor_of(snapshot.gpus.front().vendor);
        }
        catch (...) {
        }
        ctx.locale = config::sunshine.locale;
        service_->set_context(std::move(ctx));
      }

      void
      run() {
        auto last_flush = clock_t::now();
        auto last_tick = clock_t::now();
        auto last_config = clock_t::now();
        bool context_done = false;
        while (true) {
          {
            std::unique_lock lock { mutex_ };
            const auto wait = service_->streams_active() ? TICK_INTERVAL : 5s;
            cv_.wait_for(lock, wait, [&] { return stopping_ || poked_; });
            if (stopping_) break;
            poked_ = false;
          }
          const auto now = clock_t::now();
          if (!context_done) {
            // the OS and the GPU are read here, not at start: the probe is not part of the host's start-up
            context_done = true;
            fill_context();
          }
          if (now - last_config >= CONFIG_RECHECK) {
            last_config = now;
            refresh_consent_cache();
          }
          if (service_->streams_active() && now - last_tick >= TICK_INTERVAL) {
            last_tick = now;
            std::function<std::vector<session_probe_t>()> probe;
            {
              std::lock_guard lock { probe_mutex };
              probe = probe_fn;
            }
            service_->tick_streams(probe ? probe() : std::vector<session_probe_t> {});
          }
          if (now - last_flush >= FLUSH_INTERVAL || service_->flush_wanted()) {
            last_flush = now;
            service_->flush();
          }
          service_->pump();
        }
        {
          std::lock_guard lock { mutex_ };
          done_ = true;
        }
        cv_.notify_all();
      }

      std::shared_ptr<service_t> service_;
      std::thread thread_;
      std::mutex mutex_;
      std::condition_variable cv_;
      bool stopping_ = false;
      bool poked_ = false;
      bool done_ = false;
    };

    std::unique_ptr<runner_t> runner;
    std::mutex lifecycle_mutex;
  }  // namespace

  void
  start() {
    std::lock_guard lifecycle { lifecycle_mutex };
    if (runner) return;
    try {
      std::string endpoint { DEFAULT_ENDPOINT };
      if (const auto snapshot = config::get_config_snapshot()) {
        endpoint = read_endpoint_from(*snapshot);
        const auto c = consent_from_snapshot(*snapshot);
        consent_cache.usage.store(c.usage);
        consent_cache.errors.store(c.errors);
      }
      service_options_t options;
      options.dir = platf::appdata() / "telemetry";
      options.endpoint = endpoint;
      service_deps_t deps;
      deps.consent = [] { return consent_t { consent_cache.usage.load(std::memory_order_relaxed), consent_cache.errors.load(std::memory_order_relaxed) }; };
      deps.post = [](const std::string &url, const std::string &body) { return http_post(url, "application/json", body); };
      auto service = std::make_shared<service_t>(std::move(options), std::move(deps));
      context_t ctx;
      ctx.version = PROJECT_VER;
      ctx.channel = channel_name();
      ctx.os = os_name();
      ctx.arch = arch_name();
      ctx.locale = config::sunshine.locale;
      service->set_context(std::move(ctx));
      {
        std::lock_guard lock { slot_mutex };
        service_slot() = service;
      }
      log_sink = boost::make_shared<log_sink_t>(boost::make_shared<log_backend_t>());
      bl::core::get()->add_sink(log_sink);
      runner = std::make_unique<runner_t>(std::move(service));
    }
    catch (...) {
      BOOST_LOG(warning) << "Telemetry could not start"sv;
    }
  }

  void
  stop() {
    std::lock_guard lifecycle { lifecycle_mutex };
    if (!runner) return;
    bool finished = true;
    try {
      if (const auto service = current()) {
        service->emit(kind_e::usage, "app_exit", json { { "uptime_s", std::chrono::duration_cast<std::chrono::seconds>(clock_t::now() - process_start).count() } });
        // events go to the disk queue (no network wait while the host is shutting down); the next start sends them
        finished = runner->stop();
        service->flush(true);
      }
      else {
        finished = runner->stop();
      }
      if (log_sink) {
        bl::core::get()->remove_sink(log_sink);
        log_sink.reset();
      }
    }
    catch (...) {
    }
    // a thread still inside a POST keeps running to its own end and still uses the runner: the runner is then left alone
    if (finished) runner.reset();
    else (void) runner.release();
    std::lock_guard lock { slot_mutex };
    service_slot().reset();
  }

  bool
  active() {
    return current() != nullptr;
  }

  void
  host_ready() {
    const auto service = current();
    if (!service) return;
    const auto start_ms = std::chrono::duration_cast<std::chrono::milliseconds>(clock_t::now() - process_start).count();
    // cold: the host started within ten minutes of the boot of the machine (the steady clock counts from boot)
    const bool cold = std::chrono::duration_cast<std::chrono::minutes>(clock_t::now().time_since_epoch()).count() < 10;
    service->emit(kind_e::usage, "app_start", json { { "cold", cold }, { "start_ms", start_ms } });
    service->emit(kind_e::experience, "startup", json { { "host_start_ms", start_ms } });
  }

  void
  emit(kind_e kind, std::string_view name, const json &props) {
    if (const auto service = current()) service->emit(kind, name, props);
  }

  void
  feature_used(std::string_view feature) {
    emit(kind_e::usage, "feature_used", json { { "feature", std::string { feature } } });
  }

  void
  consent_changed() {
    refresh_consent_cache();
    if (const auto service = current()) service->consent_changed();
  }

  void
  settings_changed(const std::map<std::string, std::string> &before, const std::map<std::string, std::string> &after) {
    const auto service = current();
    if (!service) return;
    std::vector<std::pair<std::string, std::string>> changes, first_writes;
    for (const auto &key : setting_keys()) {
      const std::string k { key };
      const auto now = after.find(k);
      if (now == after.end()) continue;
      const auto was = before.find(k);
      const std::string old_value = was == before.end() ? std::string {} : was->second;
      if (old_value == now->second) continue;
      const auto bucket = setting_value_bucket(key, now->second);
      if (bucket.empty() || bucket == setting_value_bucket(key, old_value)) continue;
      (was == before.end() ? first_writes : changes).emplace_back(k, bucket);
    }
    // A key that was not in the file before is either a change away from the default or a default written for the first time; the first save of a fresh install writes many of those at once, which is
    // not a change of anything. A few at a time are reported, a dump of many is not.
    if (first_writes.size() <= 3) changes.insert(changes.end(), first_writes.begin(), first_writes.end());
    for (const auto &[k, bucket] : changes) service->emit(kind_e::usage, "setting_changed", json { { "key", k }, { "value", bucket } });
  }

  bool
  usage_enabled() {
    return consent_cache.usage.load(std::memory_order_relaxed);
  }

  bool
  errors_enabled() {
    return consent_cache.errors.load(std::memory_order_relaxed);
  }

  std::optional<json>
  identity() {
    const auto service = current();
    return service ? service->identity() : std::nullopt;
  }

  std::string
  install_id() {
    const auto service = current();
    return service ? service->install_id() : std::string {};
  }

  std::string
  endpoint() {
    const auto service = current();
    return service ? service->endpoint() : std::string { DEFAULT_ENDPOINT };
  }

  json
  status() {
    json out {
      { "v", WIRE_VERSION },
      { "active", false },
      { "usage", usage_enabled() },
      { "errors", errors_enabled() },
      { "endpoint", endpoint() },
    };
    if (const auto service = current()) {
      const auto counts = service->counts();
      out["active"] = true;
      out["buffered"] = service->buffered();
      out["queued_batches"] = service->queued_files();
      out["streams_active"] = service->streams_active();
      out["failures_in_a_row"] = service->consecutive_failures();
      out["counts"] = json {
        { "emitted", counts.emitted },
        { "refused", counts.refused },
        { "dropped", counts.dropped },
        { "sent_batches", counts.sent_batches },
        { "sent_events", counts.sent_events },
        { "rejected_batches", counts.rejected_batches },
        { "failed_posts", counts.failed_posts },
      };
    }
    return out;
  }

  json
  preview() {
    const auto service = current();
    // the events and the fields every batch carries with them: what leaves this computer, exactly (no event of the web or the panel is added here that the host would refuse)
    json out { { "events", service ? service->preview() : json::array() }, { "endpoint", endpoint() + "/v1/events" } };
    if (service) {
      const auto context = service->context();
      out["envelope"] = envelope_fields(context, "host", context.version);
    }
    return out;
  }

  std::size_t
  ingest(std::string_view product, std::string_view version, const json &events) {
    const auto service = current();
    return service ? service->ingest(product, version, events) : 0;
  }

  int
  post_to(std::string_view path, std::string_view content_type, const std::string &body) {
    return http_post(endpoint() + std::string { path }, std::string { content_type }, body).status;
  }

  void
  stream_started(const stream_info_t &info) {
    const auto service = current();
    if (!service) return;
    stream_dims_t dims;
    dims.client_kind = client_kind_of(info.client_family);
    dims.client_os = client_os_of(info.client_os);
    dims.codec = codec_of_video_format(info.video_format);
    dims.width = info.width;
    dims.height = info.height;
    dims.fps = info.fps;
    dims.hdr = info.hdr;
    // an application with a sparxie block is a Sparxie application however it is started (a direct start runs the game's own exe, not sparxie-launch)
    dims.app_kind = info.sparxie_app ? "sparxie" : classify_app_kind(info.app_name, info.app_cmd);
    dims.transport = transport_of(info.altp);
    if (info.sparxie_app) dims.sparxie_game = sparxie_game_id(info.sparxie_game);
    service->stream_begin(dims, info.session_id, info.client_uuid);
    if (info.hdr) feature_used("stream.hdr");
    if (info.mic) feature_used("stream.mic");
    if (info.vdd) feature_used("stream.vdd");
    if (dims.app_kind == "sparxie") feature_used("stream.sparxie");
  }

  void
  stream_ended(std::uint32_t session_id, std::string_view stop_reason) {
    if (const auto service = current()) service->stream_end(session_id, stop_reason);
  }

  void
  frame_encoded(std::uint32_t session_id) noexcept {
    if (const auto service = current()) service->record_encoded_frame(session_id);
  }

  void
  encode_ms(std::uint32_t session_id, double ms) noexcept {
    if (const auto service = current()) service->record_encode_ms(session_id, ms);
  }

  void
  capture_ms(std::uint32_t session_id, double ms) noexcept {
    if (const auto service = current()) service->record_capture_ms(session_id, ms);
  }

  void
  set_session_probe(std::function<std::vector<session_probe_t>()> probe) {
    std::lock_guard lock { probe_mutex };
    probe_fn = std::move(probe);
  }

}  // namespace telemetry
