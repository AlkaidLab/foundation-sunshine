/**
 * @file src/restart_reason.h
 * @brief Why the core is being restarted: one word from a fixed list, set by whoever asks and read when the core leaves its note for the service.
 *
 * The service reports restarts the core asked for as expected, never as an anomaly; the word says what asked (the Web UI page that saved a setting, the first-run guide, the tray ...),
 * so a restart that repeats can be traced to the flow that causes it. Std only.
 */
#pragma once

#include <atomic>
#include <string_view>

namespace restart_reason {

  inline constexpr std::string_view WORDS[] = { "settings", "guide", "troubleshooting", "doctor", "home", "shell", "tray", "widget", "import", "api" };

  inline std::atomic<const char *> &
  slot() {
    static std::atomic<const char *> value { "api" };
    return value;
  }

  /** A word of the list (anything else, or nothing, is "api": a client that did not say). */
  inline void
  set(std::string_view word) {
    for (const auto w : WORDS)
      if (w == word) {
        slot().store(w.data(), std::memory_order_relaxed);  // the list is static storage and its entries are NUL-terminated literals
        return;
      }
    slot().store("api", std::memory_order_relaxed);
  }

  inline const char *
  current() {
    return slot().load(std::memory_order_relaxed);
  }

}  // namespace restart_reason
