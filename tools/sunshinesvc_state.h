/**
 * @file tools/sunshinesvc_state.h
 * @brief Platform-independent lifecycle policy for the Windows GUI agent supervisor.
 */
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

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
    adopt,  ///< one Sunshine.exe of this install is already running; do not start a second
  };

  inline core_start_e
  core_start_decision(bool already_running) {
    return already_running ? core_start_e::adopt : core_start_e::launch;
  }

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
