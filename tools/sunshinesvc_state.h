/**
 * @file tools/sunshinesvc_state.h
 * @brief Platform-independent lifecycle policy for the Windows GUI agent supervisor.
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sunshinesvc {

  // ERROR_ALREADY_EXISTS and ERROR_ACCESS_DENIED. A named mutex that already
  // exists returns a handle with the first; a caller that may not open it gets
  // a null handle and the second (CreateMutexW, Object Names).
  inline constexpr unsigned long instance_error_already_exists = 183;
  inline constexpr unsigned long instance_error_access_denied = 5;

  enum class instance_claim_e {
    owner,  ///< this process created the mutex and must keep the handle
    other_running,  ///< another process of this installation holds it, or holds it where we cannot open it
    unknown,  ///< the mutex could not be created; do not block startup on that
  };

  inline instance_claim_e
  instance_claim(bool created, unsigned long last_error) {
    if (!created) {
      if (last_error == instance_error_access_denied) return instance_claim_e::other_running;
      return instance_claim_e::unknown;
    }
    if (last_error == instance_error_already_exists) return instance_claim_e::other_running;
    return instance_claim_e::owner;
  }

  enum class core_start_e {
    launch,
    adopt,  ///< one Sunshine.exe of this install is already running and healthy; do not start a second
  };

  /// @param adoptable A running core that has finished starting and is not shutting down.
  inline core_start_e
  core_start_decision(bool adoptable) {
    return adoptable ? core_start_e::adopt : core_start_e::launch;
  }

  enum class core_event_e {
    ready,  ///< the core finished starting
    stopping,  ///< the core began to shut down
  };

  /// Per-process event a core publishes in the global namespace for the service, which runs in another session
  /// ([dr-ms-kernel-namespaces-20261109]). The process id is in the name: the events of a core that is gone vanish
  /// with it, and the cores of one install never share a state.
  inline std::wstring
  core_event_name(core_event_e kind, unsigned long process_id) {
    std::wstring name = L"Global\\FoundationSunshineCore";
    name += kind == core_event_e::ready ? L"Ready-" : L"Stopping-";
    name += std::to_wstring(process_id);
    return name;
  }

  struct core_state_t {
    bool ready = false;
    bool stopping = false;
  };

  enum class core_action_e {
    keep,  ///< supervise this core
    wait_for_exit,  ///< it is shutting down: let it finish, then end it if it does not
    end,  ///< not known to be healthy (still starting, or a build that publishes no state): end it and start a fresh one
  };

  struct core_plan_t {
    std::optional<std::size_t> keep;
    std::vector<core_action_e> actions;  ///< one per core, in the order given
  };

  /// A core is adopted only when it says it is ready and is not stopping. An adopted core that is already in its
  /// shutdown exits on its own a moment later and leaves the service without a core, so it is never adopted; a core that
  /// publishes nothing (an older build) cannot be told from one in trouble and is replaced.
  inline core_plan_t
  plan_cores(const std::vector<core_state_t> &cores) {
    core_plan_t plan;
    for (std::size_t index = 0; index < cores.size(); ++index) {
      if (!plan.keep && cores[index].ready && !cores[index].stopping) {
        plan.keep = index;
        plan.actions.push_back(core_action_e::keep);
      }
      else {
        plan.actions.push_back(cores[index].stopping ? core_action_e::wait_for_exit : core_action_e::end);
      }
    }
    return plan;
  }

  /// A core in its shutdown has this long (its own deadline is 10 s) before it is ended.
  inline constexpr std::uint32_t core_stopping_wait_ms = 12000;
  /// A core asked to end gets this long to do so before it is ended.
  inline constexpr std::uint32_t core_end_wait_ms = 5000;

  /// ASCII case and slash folding for an image path. Trailing separators are dropped so
  /// `C:\App\Sunshine.exe` and `c:/App/Sunshine.exe` name the same file.
  inline std::wstring
  normalize_win_path(std::wstring_view path) {
    std::wstring out;
    out.reserve(path.size());
    for (wchar_t c : path) {
      if (c == L'/') c = L'\\';
      if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
      out.push_back(c);
    }
    while (out.size() > 3 && (out.back() == L'\\' || out.back() == L'/')) out.pop_back();
    return out;
  }

  inline bool
  same_install_core(std::wstring_view image, std::wstring_view expected_exe) {
    return normalize_win_path(image) == normalize_win_path(expected_exe);
  }

  inline std::uint64_t
  path_hash(std::wstring_view path) {
    // FNV-1a over the folded UTF-16 code units. The name only has to be stable for one path.
    std::uint64_t hash = 14695981039346656037ull;
    const auto folded = normalize_win_path(path);
    for (wchar_t c : folded) {
      hash ^= static_cast<unsigned char>(c & 0xff);
      hash *= 1099511628211ull;
      hash ^= static_cast<unsigned char>((static_cast<unsigned>(c) >> 8) & 0xff);
      hash *= 1099511628211ull;
    }
    return hash;
  }

  /// Cross-session single-instance name. The global namespace is what a service in session 0
  /// and a user process in session 1 both see (Kernel object namespaces).
  inline std::wstring
  instance_mutex_name(std::wstring_view exe_path) {
    static constexpr wchar_t digits[] = L"0123456789abcdef";
    auto hash = path_hash(exe_path);
    std::wstring name = L"Global\\FoundationSunshineHost-";
    for (int shift = 60; shift >= 0; shift -= 4) {
      name.push_back(digits[(hash >> shift) & 0xf]);
    }
    return name;
  }

  constexpr std::uint32_t GUI_RESTART_INITIAL_DELAY_MS = 1000;
  constexpr std::uint32_t GUI_RESTART_MAX_DELAY_MS = 30000;
  constexpr std::uint64_t GUI_RESTART_STABLE_RUNTIME_MS = 60000;
  constexpr std::uint32_t GUI_REATTACH_POLL_MS = 3000;
  constexpr std::uint64_t GUI_CRASH_LOG_REMINDER_MS = 300000;
  constexpr std::uint32_t GUI_WEBVIEW2_MISSING_EXIT_CODE = 0x5352;
  constexpr std::uint32_t GUI_WEBVIEW2_MISSING_MAX_FAILURES = 3;

  // The GUI writes this with ExitProcess only for a deliberate menu quit and
  // for the updater handoff. It is not 0 or 1: GetExitCodeProcess reports the
  // caller's TerminateProcess code when Task Manager or the taskbar End task
  // ends the process, and that path does not run the menu quit.
  // 259 is STILL_ACTIVE and must not be used as an exit code.
  // [ms-getexitcodeprocess-20261109], [ms-terminating-a-process-20261109],
  // [ms-terminateprocess-20261109], [ms-registerapplicationrestart-20261109]
  inline constexpr std::uint32_t GUI_AGENT_DELIBERATE_QUIT_EXIT_CODE = 0x5351;

  enum class GuiAgentExitAction {
    relaunch,
    suppress_launch,
    adopt_replacement,
  };

  // replacement_running is another agent of this install already in the session.
  // That covers the elevated-restart handoff and a single-instance duplicate.
  inline GuiAgentExitAction
  decide_gui_agent_exit(const std::uint32_t exit_code, const bool replacement_running) {
    if (replacement_running) {
      return GuiAgentExitAction::adopt_replacement;
    }
    if (exit_code == GUI_AGENT_DELIBERATE_QUIT_EXIT_CODE) {
      return GuiAgentExitAction::suppress_launch;
    }
    return GuiAgentExitAction::relaunch;
  }

  class GuiRestartBackoff {
  public:
    std::uint32_t
    next_delay(std::uint64_t previous_runtime_ms = 0) {
      if (previous_runtime_ms >= GUI_RESTART_STABLE_RUNTIME_MS) {
        failures_ = 0;
      }

      const auto shift = std::min(failures_, 5U);
      const auto delay = std::min(
        GUI_RESTART_INITIAL_DELAY_MS << shift,
        GUI_RESTART_MAX_DELAY_MS);
      ++failures_;
      return delay;
    }

    void
    reset() {
      failures_ = 0;
    }

  private:
    std::uint32_t failures_ = 0;
  };

  /// A core that ended without being asked, and sooner than this after it started, counts as a failure of the start (not of a stream).
  constexpr std::uint64_t CORE_STABLE_RUNTIME_MS = 60000;
  constexpr std::uint32_t CORE_RESTART_FIRST_DELAY_MS = 5000;
  constexpr std::uint32_t CORE_RESTART_MAX_DELAY_MS = 60000;
  constexpr std::uint32_t CORE_RESTART_FREE_FAILURES = 2;

  /**
   * @brief How long the service waits before it starts the core again after a core that failed at its start.
   *
   * One install ran 29 cores in a row, each ended by an access violation about 1.3 s after it started (a crash inside the graphics driver at start-up, the
   * crashed thread was the driver's own), a launch every 3 s with a crash report each. The first failures are retried at once as before (a single
   * failure is often a driver that was not ready); from the third quick failure on the wait grows 5, 10, 20, 40 and 60 s, and a core that runs for a minute,
   * or one that ended because it was asked to (a restart, a stop, a change of session), starts the count again.
   */
  class CoreRestartBackoff {
  public:
    /** @return the wait in ms before the next start, on top of the service's own pass interval; 0 when the core is to be started as usual. */
    std::uint32_t
    next_delay(std::uint64_t runtime_ms, bool asked_for) {
      if (asked_for || runtime_ms >= CORE_STABLE_RUNTIME_MS) {
        failures_ = 0;
        return 0;
      }
      ++failures_;
      if (failures_ <= CORE_RESTART_FREE_FAILURES) {
        return 0;
      }
      const auto shift = std::min(failures_ - CORE_RESTART_FREE_FAILURES - 1, 4U);
      return std::min(CORE_RESTART_FIRST_DELAY_MS << shift, CORE_RESTART_MAX_DELAY_MS);
    }

    void
    reset() {
      failures_ = 0;
    }

  private:
    std::uint32_t failures_ = 0;
  };

  /** What the service waits before it starts the core again. */
  struct CoreRestartWait {
    std::uint32_t delay_ms = 0;  ///< the wait before the next start; 0: the core is started as usual in the next pass, and no log line is written
    std::uint64_t not_before_tick = 0;  ///< the tick the next start waits for, written for every end of a core (a zero wait included)
  };

  /**
   * @brief The wait after one end of a core, and the tick the next start is allowed from.
   *
   * The deadline is written for every end, also when the wait is zero: a core that ran for a minute, or one that ended because it was asked to, resets the count,
   * and the deadline of the failures before it must not hold back the start that follows (after the service adopts an external core, an active restart was kept
   * waiting by the old deadline for up to CORE_RESTART_MAX_DELAY_MS). The log line is written for a positive wait only.
   */
  inline CoreRestartWait
  core_restart_wait(CoreRestartBackoff &backoff, std::uint64_t runtime_ms, bool asked_for, std::uint64_t now_tick) {
    const auto delay_ms = backoff.next_delay(runtime_ms, asked_for);
    return { delay_ms, now_tick + delay_ms };
  }

  class GuiAgentRestartPolicy {
  public:
    bool
    note_webview2_missing(std::uint64_t previous_runtime_ms) {
      if (previous_runtime_ms >= GUI_RESTART_STABLE_RUNTIME_MS) {
        webview2_missing_failures_ = 0;
      }
      ++webview2_missing_failures_;
      return webview2_missing_failures_ >= GUI_WEBVIEW2_MISSING_MAX_FAILURES;
    }

    void
    reset_webview2_missing() {
      webview2_missing_failures_ = 0;
    }

    void
    suppress_launch() {
      launch_suppressed_ = true;
    }

    void
    resume_supervision() {
      launch_suppressed_ = false;
    }

    bool
    launch_allowed() const {
      return !launch_suppressed_;
    }

  private:
    bool launch_suppressed_ = false;
    std::uint32_t webview2_missing_failures_ = 0;
  };

  class GuiAgentCrashLogLimiter {
  public:
    bool
    should_log_exit(std::uint32_t exit_code, std::uint32_t retry_delay_ms, std::uint64_t now_ms) {
      const bool changed = !has_last_exit_ ||
                           exit_code != last_exit_code_ ||
                           retry_delay_ms != last_retry_delay_ms_;
      const bool reminder_due = has_last_exit_ &&
                                now_ms - last_exit_log_at_ms_ >= GUI_CRASH_LOG_REMINDER_MS;
      const bool should_log = changed || reminder_due;
      has_last_exit_ = true;
      last_exit_code_ = exit_code;
      last_retry_delay_ms_ = retry_delay_ms;
      if (should_log) {
        last_exit_log_at_ms_ = now_ms;
      }
      acquisition_log_pending_ = should_log;
      return should_log;
    }

    bool
    should_log_acquisition(bool recovered_from_error) {
      const bool should_log = acquisition_log_pending_ || recovered_from_error;
      acquisition_log_pending_ = false;
      return should_log;
    }

    void
    reset() {
      has_last_exit_ = false;
      last_exit_code_ = 0;
      last_retry_delay_ms_ = 0;
      last_exit_log_at_ms_ = 0;
      acquisition_log_pending_ = true;
    }

  private:
    bool has_last_exit_ = false;
    bool acquisition_log_pending_ = true;
    std::uint32_t last_exit_code_ = 0;
    std::uint32_t last_retry_delay_ms_ = 0;
    std::uint64_t last_exit_log_at_ms_ = 0;
  };

}  // namespace sunshinesvc
