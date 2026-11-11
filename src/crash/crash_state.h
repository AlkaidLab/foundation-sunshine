/**
 * @file src/crash/crash_state.h
 * @brief State shared by the crash handlers, the watchdog and the uploader (internal to src/crash).
 *
 * Plain fixed buffers and atomics: the handlers read this while the process may be in any state, so nothing here allocates or locks.
 */
#pragma once

#include "crash.h"
#include "crash_core.h"

#include <atomic>
#include <cctype>
#include <string>
#include <cstdint>

namespace crash::detail {
  constexpr int max_slots = 12;
  constexpr std::size_t path_cap = 1040;

  struct slot_t {
    std::atomic<bool> used { false };
    std::atomic<std::uint64_t> busy_since_ms { 0 };  ///< 0 = idle
    std::uint32_t timeout_ms = 0;
    char name[32] = {};
    std::atomic<bool> reported { false };
    std::uint64_t thread_token = 0;  ///< platform: the duplicated thread handle, or the kernel thread id
  };

  struct state_t {
    char product[32] = "host";
    char version[96] = {};
    char os_version[64] = {};
    char pending_utf8[path_cap] = {};
    std::atomic<bool> installed { false };
    std::atomic<bool> enabled { true };
    std::int64_t start_unix = 0;
    std::uint64_t start_ms = 0;
    slot_t slots[max_slots];
  };

  extern state_t g;

  std::uint64_t
  now_ms();  ///< steady, monotonic milliseconds
  std::int64_t
  now_unix();

  /// "<unix>-<product>-<kind>-p<pid hex>" into out; no allocation.
  std::size_t
  stem_noalloc(char *out, std::size_t cap, std::int64_t unix_seconds, const char *kind, std::uint32_t pid);

  // implemented per platform ------------------------------------------------------------------------------------------------------------
  void
  platform_install();  ///< handlers, worker thread, module snapshot
  void
  platform_tick();  ///< every watchdog tick (about 1 s): refreshes the module snapshot now and then, re-asserts the exception filter
  /// A thread stayed busy past its timeout: write a "hang" report for it. Runs on the watchdog thread.
  void
  platform_report_hang(int slot);
  /// Remember the calling thread for a slot so a hang report can read its stack.
  void
  platform_bind_thread(slot_t &slot);
  void
  platform_release_thread(slot_t &slot);
  void
  platform_test_crash(const char *what);
}  // namespace crash::detail

namespace crash {
  std::string
  current_conf_text();
  std::filesystem::path
  current_config_dir();
  void
  stop_watchdog();
}  // namespace crash
