/**
 * @file src/platform/windows/gui_agent_supervisor.cpp
 * @brief Keep a tray in the user session when Core is not the Windows service.
 *
 * The service wrapper watches the GUI agent while Core runs as SYSTEM. A
 * standalone or portable Core process is the user, so it has to watch the
 * agent itself. RegisterApplicationRestart does not: it only offers a restart
 * after an unhandled exception or a hang, with the user's consent, and only
 * after 60 seconds.
 * [ms-registerapplicationrestart-20261109]
 *
 * Ending the process from Task Manager or the taskbar End task is
 * TerminateProcess. The exit code is the caller's, and the process runs no
 * further code, so it cannot be told apart from a menu quit by exit code 0.
 * [ms-terminateprocess-20261109], [ms-terminating-a-process-20261109],
 * [ms-getexitcodeprocess-20261109]. A running replacement is found with
 * CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS) [ms-createtoolhelp32snapshot-20261109].
 */
#include "src/platform/windows/misc.h"

#include "src/logging.h"
#include "tools/sunshinesvc_state.h"

#include <mutex>
#include <string>
#include <thread>

// clang-format off
#include <windows.h>
#include <tlhelp32.h>
// clang-format on

namespace {
  std::mutex lifecycle_mutex;
  std::thread worker;
  HANDLE stop_event = nullptr;
  bool running = false;

  struct agent_process_t {
    HANDLE handle = nullptr;
    DWORD process_id = 0;
  };

  void
  close_agent(agent_process_t &agent) {
    if (agent.handle != nullptr) {
      CloseHandle(agent.handle);
    }
    agent = {};
  }

  bool
  stop_requested() {
    return stop_event != nullptr && WaitForSingleObject(stop_event, 0) == WAIT_OBJECT_0;
  }

  bool
  wait_for_stop(DWORD timeout_ms) {
    if (stop_event == nullptr) {
      return true;
    }
    return WaitForSingleObject(stop_event, timeout_ms) == WAIT_OBJECT_0;
  }

  std::wstring
  installed_gui_path() {
    std::wstring executable(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size()) {
      return {};
    }
    executable.resize(length);
    const auto slash = executable.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
      return {};
    }
    return executable.substr(0, slash) + L"\\assets\\gui\\sunshine-gui.exe";
  }

  agent_process_t
  find_running_agent(DWORD exclude_process_id) {
    agent_process_t found;
    const auto gui_path = installed_gui_path();
    if (gui_path.empty()) {
      return found;
    }
    DWORD session_id = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session_id)) {
      return found;
    }

    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
      return found;
    }

    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot, &entry)) {
      CloseHandle(snapshot);
      return found;
    }

    while (true) {
      const bool named = _wcsicmp(entry.szExeFile, L"sunshine-gui.exe") == 0;
      DWORD candidate_session = 0;
      if (named && entry.th32ProcessID != exclude_process_id &&
          ProcessIdToSessionId(entry.th32ProcessID, &candidate_session) && candidate_session == session_id) {
        auto candidate = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (candidate != nullptr) {
          std::wstring path(32768, L'\0');
          auto path_length = static_cast<DWORD>(path.size());
          const bool same_install = QueryFullProcessImageNameW(candidate, 0, path.data(), &path_length) &&
                                    _wcsicmp(std::wstring(path.data(), path_length).c_str(), gui_path.c_str()) == 0;
          const bool alive = same_install && WaitForSingleObject(candidate, 0) != WAIT_OBJECT_0;
          if (alive) {
            found.handle = candidate;
            found.process_id = entry.th32ProcessID;
            break;
          }
          CloseHandle(candidate);
        }
      }

      if (!Process32NextW(snapshot, &entry)) {
        break;
      }
    }

    CloseHandle(snapshot);
    return found;
  }

  void
  supervisor_main() {
    sunshinesvc::GuiRestartBackoff backoff;
    sunshinesvc::GuiAgentRestartPolicy restart_policy;
    agent_process_t agent;
    ULONGLONG acquired_at_ms = 0;

    while (!stop_requested()) {
      if (agent.handle == nullptr) {
        agent = find_running_agent(0);
        if (agent.handle == nullptr && restart_policy.launch_allowed()) {
          std::error_code error;
          agent.handle = platf::launch_gui_agent_process(error);
          if (agent.handle != nullptr) {
            agent.process_id = GetProcessId(agent.handle);
          }
        }
        if (agent.handle == nullptr) {
          if (!restart_policy.launch_allowed()) {
            if (wait_for_stop(sunshinesvc::GUI_REATTACH_POLL_MS)) {
              break;
            }
            continue;
          }
          const auto delay_ms = backoff.next_delay();
          BOOST_LOG(warning) << "GUI agent is not running and could not be started; retrying in " << delay_ms << " ms";
          if (wait_for_stop(delay_ms)) {
            break;
          }
          continue;
        }
        acquired_at_ms = GetTickCount64();
        backoff.reset();
      }

      const HANDLE waits[] = { stop_event, agent.handle };
      const auto waited = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
      if (waited != WAIT_OBJECT_0 + 1) {
        break;
      }

      DWORD exit_code = ERROR_PROCESS_ABORTED;
      if (!GetExitCodeProcess(agent.handle, &exit_code) || exit_code == STILL_ACTIVE) {
        exit_code = ERROR_PROCESS_ABORTED;
      }
      const auto exit_at_ms = GetTickCount64();
      const auto runtime_ms = exit_at_ms - acquired_at_ms;
      const auto exited_process_id = agent.process_id;
      close_agent(agent);

      auto replacement = find_running_agent(exited_process_id);
      const bool replacement_running = replacement.handle != nullptr;
      const bool suppress_missing_webview =
        !replacement_running && exit_code == sunshinesvc::GUI_WEBVIEW2_MISSING_EXIT_CODE &&
        restart_policy.note_webview2_missing(runtime_ms);
      if (!replacement_running && exit_code != sunshinesvc::GUI_WEBVIEW2_MISSING_EXIT_CODE) {
        restart_policy.reset_webview2_missing();
      }
      const auto action = suppress_missing_webview ? sunshinesvc::GuiAgentExitAction::suppress_launch : sunshinesvc::decide_gui_agent_exit(exit_code, replacement_running);
      if (action == sunshinesvc::GuiAgentExitAction::adopt_replacement) {
        agent = replacement;
        acquired_at_ms = exit_at_ms;
        restart_policy.resume_supervision();
        backoff.reset();
        BOOST_LOG(info) << "GUI agent PID " << exited_process_id << " exited with code " << exit_code
                        << "; watching replacement PID " << agent.process_id;
        continue;
      }
      close_agent(replacement);

      if (action == sunshinesvc::GuiAgentExitAction::suppress_launch) {
        restart_policy.suppress_launch();
        if (suppress_missing_webview) {
          BOOST_LOG(info) << "WebView2 Runtime missing; GUI restart suppressed after "
                          << sunshinesvc::GUI_WEBVIEW2_MISSING_MAX_FAILURES << " failures";
        }
        else {
          BOOST_LOG(info) << "GUI agent PID " << exited_process_id
                          << " quit deliberately; automatic launch suppressed";
        }
        while (!wait_for_stop(sunshinesvc::GUI_REATTACH_POLL_MS)) {
          auto again = find_running_agent(0);
          if (again.handle != nullptr) {
            agent = again;
            acquired_at_ms = GetTickCount64();
            restart_policy.resume_supervision();
            backoff.reset();
            BOOST_LOG(info) << "GUI agent reattached to PID " << agent.process_id;
            break;
          }
        }
        continue;
      }

      const auto delay_ms = backoff.next_delay(runtime_ms);
      BOOST_LOG(info) << "GUI agent PID " << exited_process_id << " exited with code " << exit_code
                      << " after " << runtime_ms << " ms; launching again in " << delay_ms << " ms";
      if (wait_for_stop(delay_ms)) {
        break;
      }
    }

    close_agent(agent);
  }
}  // namespace

namespace platf {
  void
  supervise_gui_agent() {
    std::lock_guard lock { lifecycle_mutex };
    if (running) {
      return;
    }
    stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stop_event == nullptr) {
      BOOST_LOG(warning) << "GUI agent supervisor could not create its stop event: " << GetLastError();
      return;
    }
    worker = std::thread(supervisor_main);
    running = true;
  }

  void
  stop_gui_agent_supervisor() {
    std::lock_guard lock { lifecycle_mutex };
    if (!running) {
      return;
    }
    SetEvent(stop_event);
    if (worker.joinable()) {
      worker.join();
    }
    CloseHandle(stop_event);
    stop_event = nullptr;
    running = false;
  }
}  // namespace platf
