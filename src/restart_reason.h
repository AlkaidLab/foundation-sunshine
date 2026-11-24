/**
 * @file src/restart_reason.h
 * @brief Why the core is being restarted: one word from a fixed list, passed with the restart call itself (platf::restart(reason)).
 *
 * The service reports restarts the core asked for as expected, never as an anomaly; the word says what asked (the Web UI page that saved a setting, the first-run guide, the tray ...),
 * so a restart that repeats can be traced to the flow that causes it. The word belongs to one call: it is an argument, not a shared variable, so two threads that ask at the same time
 * cannot swap their reasons. Std only.
 */
#pragma once

#include <string_view>

namespace restart_reason {

  inline constexpr std::string_view WORDS[] = { "settings", "guide", "troubleshooting", "doctor", "home", "shell", "tray", "widget", "import", "api" };

  /** A word of the list (anything else, or nothing, is "api": a client that did not say). */
  inline std::string_view
  normalize(std::string_view word) {
    for (const auto w : WORDS)
      if (w == word) return w;
    return "api";
  }

}  // namespace restart_reason
