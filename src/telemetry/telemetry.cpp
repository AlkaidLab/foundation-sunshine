/**
 * @file src/telemetry/telemetry.cpp
 * @brief Telemetry v1 in the host: the sender thread, the HTTP transport, the log tally and the glue to the config.
 */
#include "telemetry.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

#ifndef _WIN32
  #include <unistd.h>
#endif
#ifdef __APPLE__
  #include <sys/sysctl.h>
  #include <sys/time.h>
#endif
#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#endif

#include <boost/log/core.hpp>
#include <boost/log/sinks/basic_sink_backend.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/log/attributes/value_extraction.hpp>

#include "error_report.h"
#include "device_facts.h"
#include "install_journal.h"
#include "service_lifecycle.h"
#include "service.h"
#include "src/build_info.h"
#include "src/crash/crash.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/logging_site.h"
#include "src/platform/common.h"
#include "src/system_info.h"
#include "transport.h"
#include "wer_event.h"

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
      return post_once(url, content_type, body, std::string { "FoundationSunshine/" } + build_info::version());
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

    // ---- device facts and the installer's journal (stage c) ------------------------------------------------------------------------------------------------

    std::int64_t
    wall_ms() {
      return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    std::string
    read_small_file(const std::filesystem::path &path, std::size_t max_bytes) {
      std::ifstream in { path, std::ios::binary };
      if (!in) return {};
      std::string data(max_bytes + 1, '\0');
      in.read(data.data(), static_cast<std::streamsize>(data.size()));
      data.resize(static_cast<std::size_t>(in.gcount()));
      if (data.size() > max_bytes) data.erase(0, data.size() - max_bytes);
      return data;
    }

#ifdef _WIN32
    /** The screens on the desktop and the highest refresh rate among them (the current mode, not the list of modes). */
    void
    probe_displays(device_facts::facts_t &facts) {
      int count = 0;
      double max_hz = 0;
      DISPLAY_DEVICEW device {};
      device.cb = sizeof(device);
      for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &device, 0); ++i) {
        if (!(device.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) || (device.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER)) continue;
        DEVMODEW mode {};
        mode.dmSize = sizeof(mode);
        if (!EnumDisplaySettingsExW(device.DeviceName, ENUM_CURRENT_SETTINGS, &mode, 0)) continue;
        ++count;
        if (mode.dmDisplayFrequency > 1) max_hz = std::max(max_hz, static_cast<double>(mode.dmDisplayFrequency));
      }
      facts.displays = count;
      if (max_hz > 0) facts.refresh_hz = device_facts::standard_refresh_hz(max_hz);
    }
#else
    void
    probe_displays(device_facts::facts_t &) {}
#endif

    /** The facts of this computer from the inventory the host already keeps (GPU, OS, adapters) and a short look at the screens. */
    device_facts::facts_t
    facts_from(const system_info::snapshot_t &snapshot) {
      device_facts::facts_t facts;
      const system_info::gpu_info_t *gpu = nullptr;
      for (const auto &g : snapshot.gpus)
        if (g.is_streaming_adapter) {
          gpu = &g;
          break;
        }
      if (!gpu && !snapshot.gpus.empty()) gpu = &snapshot.gpus.front();
      if (gpu) {
        facts.gpu_vendor = gpu_vendor_of(gpu->vendor);
        for (const char c : gpu->driver_version)
          if ((c >= '0' && c <= '9') || c == '.') facts.gpu_driver.push_back(c);
        if (facts.gpu_driver.size() > 24) facts.gpu_driver.resize(24);
      }
      facts.os_build = device_facts::os_build_token(snapshot.os.build.empty() ? snapshot.os.version : snapshot.os.build);
      std::vector<device_facts::nic_t> nics;
      for (const auto &n : snapshot.nics) nics.push_back({ std::string { system_info::to_string(n.type) }, n.up, n.link_speed_mbps, n.is_default_route });
      if (const auto nic = device_facts::pick_nic(nics)) {
        facts.nic_link = device_facts::nic_link_class(nic->type);
        facts.nic_speed = device_facts::nic_speed_class(nic->link_speed_mbps);
      }
      probe_displays(facts);
      return facts;
    }

    /** Reports the facts when they are new or changed (and the last report is a day old): one small event, remembered in <dir>/device_facts.json. */
    void
    report_device_facts(service_t &service, const std::filesystem::path &dir, const device_facts::facts_t &facts) {
      std::optional<device_facts::state_t> last;
      const auto path = dir / "device_facts.json";
      try {
        last = device_facts::state_from_json(json::parse(read_small_file(path, 4096), nullptr, false));
      }
      catch (...) {
      }
      const auto now = wall_ms();
      if (!device_facts::should_send(last, facts, now)) return;
      service.emit(kind_e::experience, "device_facts", device_facts::to_props(facts));
      try {
        std::filesystem::create_directories(dir);
        std::ofstream out { path, std::ios::binary | std::ios::trunc };
        out << device_facts::state_to_json({ device_facts::fingerprint(facts), now }).dump();
      }
      catch (...) {
      }
    }

    /** The installer's journal becomes install_result and install_summary events and is deleted; a run that is still going is left for the next start. */
    void
    report_install_journal(service_t &service, const std::filesystem::path &dir) {
      try {
        const auto path = dir / "install_journal.jsonl";
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) return;
        const auto written = std::filesystem::last_write_time(path, ec);
        if (ec) return;
        const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(std::filesystem::file_time_type::clock::now() - written).count();
        const auto result = install_journal::events_from_journal(read_small_file(path, install_journal::MAX_BYTES), age);
        if (!result.consumed) return;
        for (const auto &item : result.items) service.emit(item.kind, item.name, item.props);
        std::filesystem::remove(path, ec);
      }
      catch (...) {
      }
    }

    // ---- the service and the start-up (service_lifecycle.h) ----------------------------------------------------------------------------------------------------

    /// When the host could serve its web interface (set by host_ready): the end of the start-up time the service measures.
    std::atomic<std::int64_t> ready_wall_ms { 0 };
    // the "core" start-up time is the difference of two readings of the steady clock (a change of the system clock in between must not show up as a slow start); the wall clock is kept for the service's own records
    std::atomic<std::int64_t> start_steady_ms { 0 };  // when telemetry started
    std::atomic<std::int64_t> ready_steady_ms { 0 };  // when the web interface became ready
    std::int64_t
    steady_ms() {
      return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    std::mutex lifecycle_state_mutex;

    service_lifecycle::state_t
    load_lifecycle_state(const std::filesystem::path &dir) {
      try {
        return service_lifecycle::state_from_json(json::parse(read_small_file(dir / "service_state.json", 8192), nullptr, false));
      }
      catch (...) {
        return {};
      }
    }

    void
    save_lifecycle_state(const std::filesystem::path &dir, const service_lifecycle::state_t &state) {
      try {
        std::filesystem::create_directories(dir);
        std::ofstream out { dir / "service_state.json", std::ios::binary | std::ios::trunc };
        out << service_lifecycle::state_to_json(state).dump();
      }
      catch (...) {
      }
    }

    long long
    own_pid() {
#ifdef _WIN32
      return static_cast<long long>(GetCurrentProcessId());
#else
      return static_cast<long long>(getpid());
#endif
    }

    /// One start-up time (service to core, or page) is judged against the limit and the install's own usual time: always a startup_timing, and a host_anomaly when it is slow.
    void
    judge_and_emit_ready(service_t &service, const std::filesystem::path &dir, std::string_view which, std::int64_t ms) {
      std::lock_guard lock { lifecycle_state_mutex };
      auto state = load_lifecycle_state(dir);
      const auto anomaly = service_lifecycle::judge_ready(which, ms, state, wall_ms());
      service.emit(kind_e::experience, "startup_timing", service_lifecycle::timing_props(which, ms, anomaly.has_value()));
      if (anomaly) service.emit(kind_e::experience, "host_anomaly", service_lifecycle::anomaly_props(*anomaly));
      save_lifecycle_state(dir, state);
    }

    /// What sunshinesvc left (tools/sunshinesvc_lifecycle.h): the counts, an XP_SVC_CORE_RESTART when the core ended without being asked, and the time from the start of the service to the web interface of this core.
    void
    report_service_lifecycle(service_t &service, const std::filesystem::path &dir) {
      try {
        const auto path = dir / "service_lifecycle.jsonl";
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) return;
        const auto records = service_lifecycle::parse(read_small_file(path, service_lifecycle::MAX_BYTES));
        std::filesystem::remove(path, ec);
        const auto summary = service_lifecycle::summarize(records);
        if (summary.launched + summary.adopted + summary.extras_ended + summary.exits + summary.restarts_requested > 0) service.emit(kind_e::experience, "svc_lifecycle", service_lifecycle::summary_props(summary));
        {
          std::lock_guard lock { lifecycle_state_mutex };
          auto state = load_lifecycle_state(dir);
          if (const auto anomaly = service_lifecycle::judge_restart(summary, state, wall_ms())) service.emit(kind_e::experience, "host_anomaly", service_lifecycle::anomaly_props(*anomaly));
          save_lifecycle_state(dir, state);
        }
        if (const auto ms = service_lifecycle::svc_to_ready_ms(records, own_pid(), ready_wall_ms.load())) judge_and_emit_ready(service, dir, "svc_core", *ms);
      }
      catch (...) {
      }
    }

    class log_backend_t: public bl::sinks::basic_sink_backend<bl::sinks::synchronized_feeding> {
    public:
      void
      consume(const bl::record_view &record) {
        // runs on the thread that logged: a counter and a copy of the line, nothing else (the line is scrubbed later, on the sender thread)
        try {
          const auto severity = record.attribute_values()["Severity"].extract<int>();
          if (!severity) return;
          const auto service = current();
          if (!service) return;
          const auto message = record.attribute_values()["Message"].extract<std::string>();
          const std::string text = message ? message.get() : std::string {};
          // every severity goes to the short ring an anomaly excerpt is cut from; only warnings and above are tallied
          service->note_log_line(severity.get(), text.c_str(), text.size());
          if (severity.get() < 3 || severity.get() > 5) return;
          service->note_log(severity.get(), text.c_str(), text.size(), component_of(text.c_str(), text.size()), severity.get() >= logging::SITE_MIN_SEVERITY ? logging::site_text(record) : std::string {});
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
              service_->set_gpu_driver(gpu.driver_version);
              break;
            }
          }
          if (ctx.gpu_vendor.empty() && !snapshot.gpus.empty()) {
            ctx.gpu_vendor = gpu_vendor_of(snapshot.gpus.front().vendor);
            service_->set_gpu_driver(snapshot.gpus.front().driver_version);
          }
          // coarse facts about the computer, once per start and only when they changed (device_facts.h)
          report_device_facts(*service_, platf::appdata() / "telemetry", facts_from(snapshot));
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
        bool lifecycle_done = false;
        bool core_timing_done = false;
        const auto run_started = clock_t::now();
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
            // what the installer left for this start (install_journal.h)
            report_install_journal(*service_, platf::appdata() / "telemetry");
          }
          // what the service left, once the web interface is up (or a minute after the start when it never says so)
          if (!lifecycle_done && context_done && (ready_wall_ms.load() != 0 || now - run_started > 60s)) {
            lifecycle_done = true;
            report_service_lifecycle(*service_, platf::appdata() / "telemetry");
          }
          // the time from the start of this core to its web interface, for every install (a host that runs without the service has no service time, and had no start-up time at all). It has its own
          // state: a web interface that is ready after the service summary was reported (a start that takes more than a minute is the one worth knowing) is still timed, once.
          if (!core_timing_done && context_done && ready_steady_ms.load() != 0) {
            core_timing_done = true;
            if (const auto ready = ready_steady_ms.load(), began = start_steady_ms.load(); began > 0 && ready >= began) judge_and_emit_ready(*service_, platf::appdata() / "telemetry", "core", ready - began);
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
          service_->pump_logs();
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

    // A word that every process of one boot of the computer sees alike and the next boot does not: the kernel's own boot id where there is one. Empty when it cannot be read (the run is then reported as "unknown", never as a restart).
    std::string
    boot_identity() {
#ifdef _WIN32
      // no boot id in the API: a volatile key of the registry is deleted by the system at every boot, so the word stored in it lives exactly as long as one boot (the first reader after the boot makes it)
      try {
        HKEY key = nullptr;
        DWORD disposition = 0;
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\FoundationSunshine\\BootId", 0, nullptr, REG_OPTION_VOLATILE, KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, &disposition) != ERROR_SUCCESS) return {};
        wchar_t word[80] = {};
        DWORD size = sizeof(word) - sizeof(wchar_t);
        DWORD type = 0;
        std::string result;
        if (RegQueryValueExW(key, L"Word", nullptr, &type, reinterpret_cast<LPBYTE>(word), &size) == ERROR_SUCCESS && type == REG_SZ && size >= sizeof(wchar_t)) {
          for (const wchar_t c : std::wstring { word })
            if (c < 128) result.push_back(static_cast<char>(c));
        }
        else {
          const auto fresh = new_uuid_v4();
          std::wstring wide(fresh.begin(), fresh.end());
          if (RegSetValueExW(key, L"Word", 0, REG_SZ, reinterpret_cast<const BYTE *>(wide.c_str()), static_cast<DWORD>((wide.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS) result = fresh;
        }
        RegCloseKey(key);
        return result;
      }
      catch (...) {
        return {};
      }
#elif defined(__APPLE__)
      char word[64] = {};
      std::size_t size = sizeof(word) - 1;
      if (sysctlbyname("kern.bootsessionuuid", word, &size, nullptr, 0) != 0) return {};
      return std::string { word };
#else
      std::ifstream in { "/proc/sys/kernel/random/boot_id" };
      std::string word;
      if (in) std::getline(in, word);
      return word;
#endif
    }
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
      options.aggregate_interval_ms = 6 * 60 * 60 * 1000;  // high-volume usage and experience records leave merged, at most every six hours and at exit
      for (const char *variable : { "COMPUTERNAME", "HOSTNAME", "USERNAME", "USER", "LOGNAME" })
        if (const char *value = std::getenv(variable)) options.redact_words.emplace_back(value);
      service_deps_t deps;
      deps.consent = [] { return consent_t { consent_cache.usage.load(std::memory_order_relaxed), consent_cache.errors.load(std::memory_order_relaxed) }; };
      deps.post = [](const std::string &url, const std::string &body) { return http_post(url, "application/json", body); };
      deps.session_log = [](const std::string &endpoint, const std::string &install_id, const std::string &version) { return crash::upload_session_log(endpoint, install_id, version); };
      deps.session_log_state = [] { return std::string { crash::session_log_state() }; };
      deps.session_shot = [](const std::string &endpoint, const std::string &install_id, const std::string &version, const std::string &image) { return crash::upload_session_shot(endpoint, install_id, version, image); };
      deps.session_shot_state = [] { return std::string { crash::session_shot_state() }; };
      deps.boot_ms = [] {
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
#ifdef _WIN32
        return static_cast<std::int64_t>(now - static_cast<std::int64_t>(GetTickCount64()));
#elif defined(__APPLE__)
        // macOS has no CLOCK_BOOTTIME: the kernel keeps the time of the boot itself
        (void) now;
        timeval boot {};
        std::size_t size = sizeof(boot);
        int name[2] = { CTL_KERN, KERN_BOOTTIME };
        if (sysctl(name, 2, &boot, &size, nullptr, 0) != 0 || boot.tv_sec <= 0) return std::int64_t { 0 };
        return static_cast<std::int64_t>(boot.tv_sec) * 1000 + boot.tv_usec / 1000;
#elif defined(CLOCK_BOOTTIME)
        timespec ts {};
        if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0) return std::int64_t { 0 };
        return static_cast<std::int64_t>(now - (static_cast<std::int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1'000'000));
#else
        (void) now;
        return std::int64_t { 0 };
#endif
      };
      deps.boot_id = [] { return boot_identity(); };
      start_steady_ms.store(steady_ms());
      auto service = std::make_shared<service_t>(std::move(options), std::move(deps));
      context_t ctx;
      ctx.version = build_info::version();
      ctx.channel = channel_name();
      ctx.os = os_name();
      ctx.arch = arch_name();
      ctx.locale = config::sunshine.locale;
      service->set_context(std::move(ctx));
      service->reconcile_consent();  // a choice that was turned off while the host was not running (the config file edited) gets its record now; the first run only remembers the choices
      service->begin_run_marker();  // the previous run did not end by itself? reported once, for that run
      {
        std::lock_guard lock { slot_mutex };
        service_slot() = service;
      }
      // the per-frame timings and counters of the streaming code reach the stream trackers through these (a null hook until now: the encode and capture sums of stream_quality were empty)
      hooks::table().frame_encoded.store(&telemetry::frame_encoded, std::memory_order_release);
      hooks::table().encode_ms.store(&telemetry::encode_ms, std::memory_order_release);
      hooks::table().capture_ms.store(&telemetry::capture_ms, std::memory_order_release);
      hooks::table().sample.store(&telemetry::record_sample, std::memory_order_release);
      hooks::table().count.store(&telemetry::record_count, std::memory_order_release);
      hooks::table().frame_done.store(&telemetry::record_frame_done, std::memory_order_release);
      log_sink = boost::make_shared<log_sink_t>(boost::make_shared<log_backend_t>());
      bl::core::get()->add_sink(log_sink);
#ifdef _WIN32
      // An install that failed earlier names its step and a code word in install-result.json (tools/installer_shell.cpp, kRunRoot): the first start after it reports each once as an error of the installer.
      if (const char *program_data = std::getenv("ProgramData"))
        for (const auto &failure : take_installer_failures(std::filesystem::path { program_data } / "FoundationSunshine" / "install-runs")) service->report_error(failure);
      // A crash of the virtual display driver leaves an Application Error event (1000) for ZakoVDD.dll. Each one is reported once, as the facts only: it happened, when (the event's time is the event's
      // own, the report carries the time of sending), the module, the exception code and the offset. The cursor is the time of the newest one taken; the first start looks back one day.
      try {
        const auto cursor_file = platf::appdata() / "telemetry" / "wer_cursor";
        std::int64_t since = 0;
        if (std::ifstream in { cursor_file }) in >> since;
        const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        if (since <= 0) since = now_ms - 24LL * 3600 * 1000;
        std::int64_t newest = since;
        for (const auto &fault : read_vdd_faults(since, 3)) {
          service->emit(kind_e::error, "wer_event", wer_props(fault), "host", {}, 0);
          newest = std::max(newest, fault.time_ms);
        }
        std::ofstream { cursor_file, std::ios::trunc } << std::max<std::int64_t>(newest, now_ms) << '\n';
      }
      catch (...) {
      }
#endif
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
    hooks::table().frame_encoded.store(nullptr, std::memory_order_release);
    hooks::table().encode_ms.store(nullptr, std::memory_order_release);
    hooks::table().capture_ms.store(nullptr, std::memory_order_release);
    hooks::table().sample.store(nullptr, std::memory_order_release);
    hooks::table().count.store(nullptr, std::memory_order_release);
    hooks::table().frame_done.store(nullptr, std::memory_order_release);
    bool finished = true;
    try {
      if (const auto service = current()) {
        service->emit(kind_e::usage, "app_exit", json { { "uptime_s", std::chrono::duration_cast<std::chrono::seconds>(clock_t::now() - process_start).count() } });
        service->end_run_marker();
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
    ready_wall_ms.store(wall_ms());
    ready_steady_ms.store(steady_ms());
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
    for (const auto &[k, bucket] : changes) {
      service->emit(kind_e::usage, "setting_changed", json { { "key", k }, { "value", bucket } });
      service->crumb(crumb_e::set, k + ":" + bucket);  // the key and the class of the new value, as in the event
    }
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
  ingest(std::string_view product, std::string_view version, const json &events, ingest_report_t *report) {
    const auto service = current();
    if (!service) return 0;
    // page_ready is the page telling when it was ready: the host judges it (limit and the install's own usual time) and sends startup_timing and, when slow, host_anomaly; the raw event is not passed on
    if (events.is_array() && (product == "panel"sv || product == "web"sv)) {
      json rest = json::array();
      for (const auto &e : events) {
        if (e.is_object() && e.value("name", "") == "page_ready") {
          const auto shaped = shape_event(kind_e::experience, "page_ready", e.contains("props") ? e["props"] : json::object(), wall_ms());
          if (shaped && shaped->props.value("page", "") == "home" && usage_enabled()) judge_and_emit_ready(*service, platf::appdata() / "telemetry", "page", shaped->props.value("ms", 0));
          continue;
        }
        rest.push_back(e);
      }
      return service->ingest(product, version, rest, report);
    }
    return service->ingest(product, version, events, report);
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
    dims.encoder = info.encoder;
    dims.capture_backend = info.capture_backend;
    if (info.sparxie_app) dims.sparxie_game = sparxie_game_id(info.sparxie_game);
    service->crumb(crumb_e::stream, "start");
    service->crumb(crumb_e::state, "codec:" + dims.codec);
    service->stream_begin(dims, info.session_id, info.client_uuid);
    if (info.hdr) feature_used("stream.hdr");
    if (info.mic) feature_used("stream.mic");
    if (info.vdd) feature_used("stream.vdd");
    if (dims.app_kind == "sparxie") feature_used("stream.sparxie");
  }

  void
  stream_ended(std::uint32_t session_id, std::string_view stop_reason, std::string_view clip_offer, std::string_view clip_flow) {
    if (const auto service = current()) {
      service->crumb(crumb_e::stream, "stop");
      service->stream_end(session_id, stop_reason, clip_offer, clip_flow);
    }
  }

  void
  set_encoder_resolver(std::function<std::string()> resolver) {
    if (const auto service = current()) service->set_encoder_resolver(std::move(resolver));
  }

  void
  stream_stage(std::string_view stage, std::int64_t ms) {
    if (const auto service = current()) service->stream_stage(stage, ms);
  }

  void
  stream_failed(std::string_view stage, std::string_view code) {
    if (const auto service = current()) service->stream_failed(stage, code);
  }

  void
  driver_failed(std::string_view driver, std::string_view op, std::string_view code) {
    if (const auto service = current()) service->driver_failed(driver, op, code);
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
  crumb(crumb_e kind, std::string_view id) {
    if (const auto service = current()) service->crumb(kind, id);
  }

  void
  report_error(std::string_view code, std::string_view severity, bool with_stack, const std::source_location &where) {
    const auto service = current();
    if (!service) return;
    try {
      error_input_t e;
      e.component = "host";
      e.code = std::string { code };
      e.severity = severity == "fatal" || severity == "warning" || severity == "info" ? std::string { severity } : "error";
      e.loc = location_of(where);
      if (!e.loc.empty()) e.frames.push_back(e.loc);
      if (with_stack)
        for (auto &f : capture_frames(2)) e.frames.push_back(std::move(f));
      e.build = std::string { build_info::version() };
      service->report_error(e);
    }
    catch (...) {
    }
  }

  void
  record_sample(std::uint32_t session_id, hooks::metric_e metric, double ms) noexcept {
    if (const auto service = current()) service->record_sample(session_id, metric, ms);
  }

  void
  record_count(std::uint32_t session_id, hooks::counter_e counter, std::uint64_t n) noexcept {
    if (const auto service = current()) service->record_count(session_id, counter, n);
  }

  void
  record_frame_done(std::uint32_t session_id, std::int64_t steady_ns) noexcept {
    if (const auto service = current()) service->record_frame_done(session_id, steady_ns);
  }

  void
  set_session_probe(std::function<std::vector<session_probe_t>()> probe) {
    std::lock_guard lock { probe_mutex };
    probe_fn = std::move(probe);
  }

}  // namespace telemetry
