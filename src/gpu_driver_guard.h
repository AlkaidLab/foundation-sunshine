/**
 * @file src/gpu_driver_guard.h
 * @brief Crash-loop protection for the start, and what the host tells the person when the failure lay in the graphics driver.
 *
 * Two pieces of state live in the config folder, both small JSON files written with a temporary file and a rename:
 *
 *  - `startup_guard.json`: how the last runs ended. A run counts as a failed start when it did not end by a shutdown request and did not stay up for
 *    `stable_after_s` (10 s), or when it hung in the driver. `loop_threshold` (3) failed starts in a row put the next start into the safe mode: it skips the encoder
 *    check and the display restore of the start (the two places a driver call has stopped the start), and everything else works as usual (a stream
 *    still checks the encoders when it is asked for). The safe mode ends after `safe_clear_after_s` (5 min) of running, when a stream has ended, or when the person
 *    asks for another try; the count then starts at zero (a try that fails again ends in the safe mode after one more failure).
 *  - `gpu_driver_notice.json`: which driver versions the person has dismissed the notice for, and which have already been shown in the tray.
 *
 * The driver mark itself (vendor, module, driver version, phase) is written by the crash handler (src/crash/gpu_driver.h).
 * Nothing here talks to a driver; every clock and folder is a parameter, so the unit tests need neither.
 *
 * Two clocks are used, each for what it is good at: the wall clock only writes the times in the files (a person or a tool reads them), and every duration that
 * decides something (how long this run has stayed up) is measured on the monotonic clock of the process. A clock that is set forward or back (a time sync after a
 * start without a network, a daylight-saving mistake, a person changing the date) can therefore neither make a failing run look stable nor keep a good one from becoming stable.
 */
#pragma once

#include "src/crash/gpu_driver.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace gpu_guard {

  inline constexpr const char *GUARD_FILE = "startup_guard.json";
  inline constexpr const char *NOTICE_FILE = "gpu_driver_notice.json";

  struct policy_t {
    int stable_after_s = 10;  ///< a run that ends before this without a shutdown request failed
    int loop_threshold = 3;  ///< failed starts in a row that put the next start into the safe mode
    int safe_clear_after_s = 300;  ///< running time in the safe mode after which it ends
  };

  /// What the encoder check of the start is doing (the Web UI and the Panel say "checking the encoders" while it is not over).
  enum class probe_e : int { pending = 0, running = 1, done = 2, failed = 3, skipped = 4 };

  constexpr const char *
  probe_word(probe_e p) {
    switch (p) {
      case probe_e::pending: return "pending";
      case probe_e::running: return "running";
      case probe_e::done: return "done";
      case probe_e::failed: return "failed";
      case probe_e::skipped: return "skipped";
    }
    return "pending";
  }

  inline std::atomic<int> g_probe { 0 };

  inline void
  set_probe(probe_e p) noexcept {
    g_probe.store(static_cast<int>(p), std::memory_order_release);
  }

  inline probe_e
  get_probe() noexcept {
    const int v = g_probe.load(std::memory_order_acquire);
    return v >= 0 && v <= 4 ? static_cast<probe_e>(v) : probe_e::pending;
  }

  inline std::int64_t
  system_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  }

  /// Seconds on the monotonic clock of this machine: it never goes back and a change of the date does not move it. Only differences mean anything.
  inline std::int64_t
  monotonic_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  }

  namespace detail {
    inline std::string
    read_text(const std::filesystem::path &path) {
      std::ifstream in(path, std::ios::binary);
      if (!in) return {};
      std::ostringstream ss;
      ss << in.rdbuf();
      return ss.str();
    }

    /// A temporary file, then a rename: a process that ends in between leaves the old file whole.
    inline bool
    write_text(const std::filesystem::path &path, const std::string &text) {
      std::error_code ec;
      std::filesystem::create_directories(path.parent_path(), ec);
      auto tmp = path;
      tmp += ".tmp";
      {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << text;
        if (!out) return false;
      }
      std::filesystem::rename(tmp, path, ec);
      return !ec;
    }

    inline nlohmann::json
    read_json(const std::filesystem::path &path) {
      const auto text = read_text(path);
      if (text.empty()) return nlohmann::json::object();
      auto j = nlohmann::json::parse(text, nullptr, false);
      return j.is_object() ? j : nlohmann::json::object();
    }

    inline std::int64_t
    int_of(const nlohmann::json &j, const char *key, std::int64_t fallback = 0) {
      const auto it = j.find(key);
      return it != j.end() && it->is_number_integer() ? it->get<std::int64_t>() : fallback;
    }

    inline bool
    bool_of(const nlohmann::json &j, const char *key, bool fallback = false) {
      const auto it = j.find(key);
      return it != j.end() && it->is_boolean() ? it->get<bool>() : fallback;
    }

    inline std::string
    string_of(const nlohmann::json &j, const char *key) {
      const auto it = j.find(key);
      return it != j.end() && it->is_string() ? it->get<std::string>() : std::string {};
    }
  }  // namespace detail

  // --- the guard ---------------------------------------------------------------------------------------------------------------------------------

  struct run_t {
    std::int64_t start = 0;  ///< 0: no run on record
    bool stable = false;  ///< stayed up for stable_after_s
    bool clean = false;  ///< ended by a shutdown request
    bool hang = false;  ///< a watched step stopped in the driver for good
  };

  struct state_t {
    int consecutive = 0;  ///< failed starts in a row, as of this start
    bool safe = false;
    std::int64_t safe_since = 0;
    run_t run;  ///< the run that is going on
  };

  inline nlohmann::json
  to_json(const state_t &s) {
    return {
      { "v", 1 },
      { "consecutive", s.consecutive },
      { "safe", s.safe },
      { "safe_since", s.safe_since },
      { "run", { { "start", s.run.start }, { "stable", s.run.stable }, { "clean", s.run.clean }, { "hang", s.run.hang } } },
    };
  }

  inline state_t
  state_from_json(const nlohmann::json &j) {
    state_t s;
    s.consecutive = static_cast<int>(std::clamp<std::int64_t>(detail::int_of(j, "consecutive"), 0, 1000));
    s.safe = detail::bool_of(j, "safe");
    s.safe_since = detail::int_of(j, "safe_since");
    if (const auto it = j.find("run"); it != j.end() && it->is_object()) {
      s.run.start = detail::int_of(*it, "start");
      s.run.stable = detail::bool_of(*it, "stable");
      s.run.clean = detail::bool_of(*it, "clean");
      s.run.hang = detail::bool_of(*it, "hang");
    }
    return s;
  }

  /// Whether the run that was on record when this one started counts as a failed start.
  inline bool
  run_failed(const run_t &previous) {
    if (previous.start == 0 || previous.clean) return false;
    return !previous.stable || previous.hang;
  }

  class guard_t {
  public:
    using clock_fn = std::function<std::int64_t()>;

    /// `clock` is the wall clock (the times that are written down), `monotonic` the clock the running time is measured on.
    explicit guard_t(std::filesystem::path dir, policy_t policy = {}, clock_fn clock = system_now, clock_fn monotonic = monotonic_now):
        path_ { dir / GUARD_FILE }, policy_ { policy }, clock_ { std::move(clock) }, monotonic_ { std::move(monotonic) } {}

    /// The start: reads how the previous run ended, decides whether this start is in the safe mode and records this run. Returns the safe mode.
    bool
    begin() {
      std::lock_guard lock(mutex_);
      state_ = state_from_json(detail::read_json(path_));
      const bool failed = run_failed(state_.run);
      state_.consecutive = failed ? state_.consecutive + 1 : 0;
      const auto now = clock_();
      if (!state_.safe && state_.consecutive >= policy_.loop_threshold) {
        state_.safe = true;
        state_.safe_since = now;
      }
      state_.run = run_t { now, false, false, false };
      watch_from_now();
      save();
      return state_.safe;
    }

    bool
    safe_mode() const {
      std::lock_guard lock(mutex_);
      return state_.safe;
    }

    /// Once a second: a run that has stayed up long enough is stable (it ends the count), and the safe mode ends after its running time.
    void
    tick() {
      std::lock_guard lock(mutex_);
      if (!watching_) return;
      const auto up = monotonic_() - watched_since_;
      bool changed = false;
      if (!state_.run.stable && up >= policy_.stable_after_s) {
        state_.run.stable = true;  // the next start sees a run that did not fail (unless it hangs later) and starts the count again
        changed = true;
      }
      if (state_.safe && up >= policy_.safe_clear_after_s) {
        leave_safe(0);
        changed = true;
      }
      if (changed) save();
    }

    /// A watched step stopped in the driver (the encoder check did not return): this run does not count as good, however long it stays up.
    void
    note_hang() {
      std::lock_guard lock(mutex_);
      if (state_.run.hang) return;
      state_.run.hang = true;
      save();
    }

    /// A shutdown was asked for: the run ended on purpose.
    void
    note_clean_exit() {
      std::lock_guard lock(mutex_);
      if (state_.run.clean) return;
      state_.run.clean = true;
      save();
    }

    /// A stream has ended: the driver works, the safe mode is over.
    void
    note_stream_finished() {
      std::lock_guard lock(mutex_);
      if (!state_.safe) return;
      leave_safe(0);
      save();
    }

    /// The person asks for another try. Returns whether the safe mode was on. The next failed start puts it back at once (the count stands one below the threshold).
    bool
    request_retry() {
      std::lock_guard lock(mutex_);
      const bool was = state_.safe;
      if (was) {
        leave_safe(std::max(0, policy_.loop_threshold - 1));
        // the try is watched from now on: a run that was already stable must not count as the success of this one
        state_.run = run_t { clock_(), false, false, false };
        watch_from_now();
        save();
      }
      return was;
    }

    state_t
    snapshot() const {
      std::lock_guard lock(mutex_);
      return state_;
    }

    const policy_t &
    policy() const {
      return policy_;
    }

  private:
    /// The running time of the run on record counts from here, on the monotonic clock (the start time in the file is the wall clock and is not used for it).
    void
    watch_from_now() {
      watched_since_ = monotonic_();
      watching_ = true;
    }

    void
    leave_safe(int consecutive) {
      state_.safe = false;
      state_.safe_since = 0;
      state_.consecutive = consecutive;
    }

    void
    save() {
      detail::write_text(path_, to_json(state_).dump());
    }

    std::filesystem::path path_;
    policy_t policy_;
    clock_fn clock_;
    clock_fn monotonic_;
    std::int64_t watched_since_ = 0;
    bool watching_ = false;
    mutable std::mutex mutex_;
    state_t state_;
  };

  /// The guard of this process (set by main; null before the start and in tools).
  inline std::atomic<guard_t *> g_guard { nullptr };

  /// Calls guard.tick() once a second until it is destroyed (a local of main: every way out of main stops and joins it).
  class ticker_t {
  public:
    explicit ticker_t(guard_t &guard):
        thread_ { [this, &guard] {
          std::unique_lock lock { mutex_ };
          while (!cv_.wait_for(lock, std::chrono::seconds(1), [this] { return stop_; })) {
            lock.unlock();
            guard.tick();
            lock.lock();
          }
        } } {}

    ~ticker_t() {
      {
        std::lock_guard lock { mutex_ };
        stop_ = true;
      }
      cv_.notify_all();
      if (thread_.joinable()) thread_.join();
    }

    ticker_t(const ticker_t &) = delete;
    ticker_t &
    operator=(const ticker_t &) = delete;

  private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::thread thread_;
  };

  // --- the driver mark and its notice --------------------------------------------------------------------------------------------------------------

  struct driver_mark_t {
    crash::gpu_driver::vendor_e vendor = crash::gpu_driver::vendor_e::none;
    std::string module;
    std::string driver;
    crash::gpu_driver::phase_e phase = crash::gpu_driver::phase_e::startup;
    std::string kind;  ///< crash | hang
    std::int64_t time = 0;

    /// Same driver, same notice: the key of a dismissal.
    std::string
    key() const {
      return std::string { crash::gpu_driver::vendor_word(vendor) } + ":" + driver;
    }
  };

  /// The mark the crash handler wrote, or nothing when there is none or it is not one (a damaged file, an unknown vendor).
  inline std::optional<driver_mark_t>
  read_mark(const std::filesystem::path &config_dir) {
    const auto j = detail::read_json(config_dir / "crashes" / crash::gpu_driver::mark_file_name);
    driver_mark_t mark;
    mark.vendor = crash::gpu_driver::vendor_from_word(detail::string_of(j, "vendor"));
    if (mark.vendor == crash::gpu_driver::vendor_e::none) return std::nullopt;
    mark.module = detail::string_of(j, "module");
    mark.driver = detail::string_of(j, "driver");
    mark.phase = crash::gpu_driver::phase_from_word(detail::string_of(j, "phase"));
    mark.kind = detail::string_of(j, "kind") == "hang" ? "hang" : "crash";
    mark.time = detail::int_of(j, "time");
    return mark;
  }

  class notice_store_t;
  /// The notice store of this process (set by main; null before the start and in tools).
  inline std::atomic<notice_store_t *> g_notices { nullptr };

  /// Which driver versions have had their notice dismissed or shown in the tray (the file lies in the config folder: the three places read the same).
  class notice_store_t {
  public:
    explicit notice_store_t(std::filesystem::path dir):
        path_ { dir / NOTICE_FILE } {}

    bool
    dismissed(const std::string &key) const {
      std::lock_guard lock(mutex_);
      return has("dismissed", key);
    }

    bool
    notified(const std::string &key) const {
      std::lock_guard lock(mutex_);
      return has("notified", key);
    }

    void
    dismiss(const std::string &key) {
      std::lock_guard lock(mutex_);
      add("dismissed", key);
    }

    void
    mark_notified(const std::string &key) {
      std::lock_guard lock(mutex_);
      add("notified", key);
    }

  private:
    static constexpr std::size_t max_keys = 32;

    bool
    has(const char *list, const std::string &key) const {
      const auto j = detail::read_json(path_);
      const auto it = j.find(list);
      if (it == j.end() || !it->is_array()) return false;
      return std::any_of(it->begin(), it->end(), [&](const nlohmann::json &v) { return v.is_string() && v.get<std::string>() == key; });
    }

    void
    add(const char *list, const std::string &key) {
      auto j = detail::read_json(path_);
      auto &arr = j[list];
      if (!arr.is_array()) arr = nlohmann::json::array();
      if (std::none_of(arr.begin(), arr.end(), [&](const nlohmann::json &v) { return v.is_string() && v.get<std::string>() == key; })) arr.push_back(key);
      while (arr.size() > max_keys) arr.erase(arr.begin());
      j["v"] = 1;
      detail::write_text(path_, j.dump());
    }

    std::filesystem::path path_;
    mutable std::mutex mutex_;
  };

  /**
   * Whether the person is told about this mark now: it has not been dismissed for this driver version, and the driver that is installed is still the one it happened in
   * (a person who updated the driver has done what the notice asks; it comes back only when the failure happens again under the new version).
   * @param installed_driver the version the system reports for the vendor now, empty when it is not known.
   */
  inline bool
  notice_due(const driver_mark_t &mark, const std::string &installed_driver, const notice_store_t &store) {
    if (!mark.driver.empty() && !installed_driver.empty() && mark.driver != installed_driver) return false;
    return !store.dismissed(mark.key());
  }

  /**
   * The state the Web UI, the Panel and the tray read (GET /api/gpu-driver-guard):
   * `{ v, probe, safe_mode: { active, since, auto_exit_s, skipped: [...] }, driver: null | { show, vendor, module, driver, phase, kind, time, key } }`.
   */
  inline nlohmann::json
  status_json(const guard_t *guard, probe_e probe, const std::optional<driver_mark_t> &mark, const std::string &installed_driver, const notice_store_t &store) {
    nlohmann::json out = { { "v", 1 }, { "probe", probe_word(probe) } };
    nlohmann::json safe = { { "active", false }, { "since", 0 }, { "auto_exit_s", 0 }, { "skipped", nlohmann::json::array() } };
    if (guard) {
      const auto state = guard->snapshot();
      if (state.safe) {
        safe["active"] = true;
        safe["since"] = state.safe_since;
        safe["auto_exit_s"] = guard->policy().safe_clear_after_s;
        safe["skipped"] = nlohmann::json::array({ "encoder_probe", "display_restore" });
      }
    }
    out["safe_mode"] = std::move(safe);
    if (mark) {
      out["driver"] = {
        { "show", notice_due(*mark, installed_driver, store) },
        { "vendor", crash::gpu_driver::vendor_word(mark->vendor) },
        { "module", mark->module },
        { "driver", mark->driver },
        { "phase", crash::gpu_driver::phase_word(mark->phase) },
        { "kind", mark->kind },
        { "time", mark->time },
        { "key", mark->key() },
      };
    }
    else {
      out["driver"] = nullptr;
    }
    return out;
  }

}  // namespace gpu_guard
