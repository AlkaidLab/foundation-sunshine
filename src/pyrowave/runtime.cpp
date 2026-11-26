/**
 * @file src/pyrowave/runtime.cpp
 * @brief Runtime gates for experimental video formats.
 */
#include "runtime.h"

#include "src/logging.h"
#include "src/utility.h"

extern "C" {
#include "third-party/moonlight-common-c/src/PyrowaveProtocol.h"
}

#ifdef _WIN32
  #ifndef NOMINMAX
  #define NOMINMAX
  #endif
  #include <windows.h>

  #include <vulkan/vulkan.h>
  #include <pyrowave.h>
#endif

#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>

using namespace std::string_view_literals;

namespace pyrowave {
  namespace {
    template<typename callback_t>
    void
    safe_probe_log(callback_t &&callback) noexcept {
      try {
        callback();
      }
      catch (...) {
        // A diagnostic failure must never turn an optional capability probe
        // into a process failure.
      }
    }

#ifdef _WIN32
    // The child gets this long to answer: creating a Vulkan instance with a device normally takes
    // a fraction of a second, and the wait stays below the client's RTSP read timeout.
    constexpr auto kProbeWait = std::chrono::seconds(5);
    constexpr auto kProbeTerminationGrace = std::chrono::seconds(2);

    // The advertisement state of this process: the probe of the first PyroWave selection decides it
    // once, and a failed probe keeps the contract out of every later DESCRIBE answer.
    probe_state_t g_probe_state;

    bool
    run_probe_process(std::uint32_t &exit_code) {
      std::wstring self(32768, L'\0');
      const DWORD length = GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size()));
      if (length == 0 || length >= self.size()) {
        safe_probe_log([] {
          BOOST_LOG(warning) << "PyroWave runtime probe: the executable path could not be read"sv;
        });
        return false;
      }
      self.resize(length);
      const std::filesystem::path probe = std::filesystem::path(self).parent_path() / L"tools" / L"alkaidlab_pyrowave_probe.exe";
      std::wstring command = L"\"" + probe.wstring() + L"\"";

      STARTUPINFOW startup {};
      startup.cb = sizeof(startup);
      PROCESS_INFORMATION process {};
      // The child inherits this process's token, session and environment, so the probe sees the
      // same Vulkan registry state and drivers as the in-process encoder device creation.
      // The child starts in its own directory, so the working directory of the host never becomes
      // part of the child's DLL search path.
      if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, probe.parent_path().c_str(), &startup, &process)) {
        safe_probe_log([] {
          BOOST_LOG(warning) << "PyroWave runtime probe: the probe tool (tools/alkaidlab_pyrowave_probe.exe) could not be started"sv;
        });
        return false;
      }
      CloseHandle(process.hThread);
      const auto close_process_handle = util::fail_guard([&process] {
        CloseHandle(process.hProcess);
      });

      const auto wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(kProbeWait).count();
      const auto wait_result = WaitForSingleObject(process.hProcess, static_cast<DWORD>(wait_ms));
      if (wait_result == WAIT_FAILED) {
        safe_probe_log([] {
          BOOST_LOG(warning) << "PyroWave runtime probe: waiting for the probe process failed"sv;
        });
        return false;
      }
      if (wait_result == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 1);
        const auto grace_ms = std::chrono::duration_cast<std::chrono::milliseconds>(kProbeTerminationGrace).count();
        WaitForSingleObject(process.hProcess, static_cast<DWORD>(grace_ms));
        safe_probe_log([&] {
          BOOST_LOG(warning) << "PyroWave runtime probe: the probe process did not answer within "
                             << std::chrono::duration_cast<std::chrono::seconds>(kProbeWait).count()
                             << " seconds and was terminated"sv;
        });
        return false;
      }

      DWORD code = 0;
      if (!GetExitCodeProcess(process.hProcess, &code)) {
        safe_probe_log([] {
          BOOST_LOG(warning) << "PyroWave runtime probe: the exit code of the probe process could not be read"sv;
        });
        return false;
      }
      exit_code = code;
      return true;
    }

    bool
    probe_once() {
      // Say how long it took, so a slow probe can be told from a slow client. It only ever runs on the
      // RTSP path of a client that actually selected PyroWave.
      const auto probe_begin = std::chrono::steady_clock::now();
      const auto log_duration = util::fail_guard([probe_begin] {
        safe_probe_log([&] {
          BOOST_LOG(info) << "PyroWave runtime probe took "sv
                          << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - probe_begin).count()
                          << " ms"sv;
        });
      });

      std::uint32_t exit_code = 0;
      if (!run_probe_process(exit_code)) {
        // The specific cause is already logged; a probe that did not complete counts as unavailable.
        return false;
      }
      if (exit_code != 0) {
        const auto reason = probe_exit_code_reason(exit_code);
        safe_probe_log([&] {
          if (reason.empty()) {
            BOOST_LOG(warning) << "PyroWave runtime probe failed: the probe process exited with code 0x"sv
                               << util::hex(exit_code).to_string_view();
          }
          else {
            BOOST_LOG(warning) << "PyroWave runtime probe failed: "sv << reason << " (exit code 0x"sv
                               << util::hex(exit_code).to_string_view() << ')';
          }
        });
        return false;
      }
      return true;
    }
#endif
  }

  bool
  probe_state_t::may_advertise() const noexcept {
    return !failed_.load(std::memory_order_acquire);
  }

  void
  probe_state_t::record_probe_result(bool available) noexcept {
    // Only a failure changes the state: the probe answers once per process, and the class keeps
    // that one-way semantic even if a future caller records a second result.
    if (!available) {
      failed_.store(true, std::memory_order_release);
    }
  }

  std::string_view
  probe_exit_code_reason(std::uint32_t exit_code) noexcept {
    switch (exit_code) {
      case 0:
        return "the runtime is available"sv;
      case 2:
        return "the PyroWave API version does not match"sv;
      case 3:
        // The C runtime ends a process with this code on abort(); the tool itself never returns it.
        return "the probe process aborted"sv;
      case 4:
        return "the default Vulkan device could not be created"sv;
      case 5:
        return "the default device does not support the required interop"sv;
      case 6:
        return "the probe process failed unexpectedly"sv;
      default:
        if (exit_code >= 0x80000000u) {
          // The process was ended by an NTSTATUS failure, an exception or the loader, instead of returning from
          // main. A defective third-party Vulkan implicit layer is the usual cause.
          return "the probe process crashed; a third-party Vulkan implicit layer may be at fault"sv;
        }
        return {};
    }
  }

  bool
  is_experimental_video_format(std::uint32_t video_format) noexcept {
    return video_format == LI_PYROWAVE_VIDEO_FORMAT;
  }

  bool
  is_server_video_format_available(std::uint32_t video_format) noexcept {
#ifdef _WIN32
    return is_experimental_video_format(video_format);
#else
    return !is_experimental_video_format(video_format);
#endif
  }

  bool
  may_advertise() noexcept {
#ifdef _WIN32
    return g_probe_state.may_advertise();
#else
    return false;
#endif
  }

  bool
  is_server_runtime_available() noexcept {
#ifdef _WIN32
    static std::once_flag probe_once_flag;
    static bool available = false;
    std::call_once(probe_once_flag, [] {
      // The gate is noexcept and the probe is optional: an exception out of it, out of building the command
      // line for instance, leaves PyroWave unavailable instead of ending the host.
      try {
        available = probe_once();
      }
      catch (...) {
        safe_probe_log([] {
          BOOST_LOG(warning) << "PyroWave runtime probe failed: an exception escaped the probe"sv;
        });
      }
      g_probe_state.record_probe_result(available);
    });
    return available;
#else
    return false;
#endif
  }

}  // namespace pyrowave
