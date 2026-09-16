/**
 * @file src/platform/linux/kscreen_backend.h
 * @brief When to talk to KDE's kscreen-doctor, and how long to wait for it.
 *
 * kscreen-doctor is a Plasma tool: it drives org.kde.KScreen over the session
 * bus. In a session that has no KScreen (niri, wlroots, plain X11) asking
 * anyway makes D-Bus activate a KScreen service that then waits for a Plasma
 * session to appear, so the process never returns on its own -- Sunshine only
 * gets control back when run_logged() kills it at the 10 s timeout. A single
 * session start issues dozens of queries (enumeration, topology, modes, HDR),
 * which is why connections block for a long time on such a box.
 *
 * Two rules keep that bounded:
 *   - a session that cannot have KScreen is detected from the environment and
 *     never probed at all (re-checked per call: Sunshine may start before the
 *     desktop does, and must pick it up once it appears), and
 *   - a probe that fails or times out arms a cooldown, so a broken KScreen
 *     costs at most one short probe per window instead of one 10 s hang per
 *     query.
 */
#pragma once

#include <chrono>
#include <string>

namespace platf::kscreen {
  /// A query must answer within this window; it is a tool that either talks to
  /// a running compositor or hangs, so waiting longer only delays the session.
  inline constexpr auto kProbeTimeout = std::chrono::milliseconds { 1500 };

  /// After a failed probe, only retry once per this window.
  inline constexpr auto kRetryCooldown = std::chrono::seconds { 20 };

  struct exec_result_t {
    int exit_code { -1 };
    std::string output;
  };

  /**
   * @brief Whether this session can have a KScreen backend at all.
   * @details KDE sets XDG_CURRENT_DESKTOP=KDE (Plasma Wayland/X11, greeter
   *          included). A session that exports NIRI_SOCKET is niri even when
   *          XDG_CURRENT_DESKTOP claims KDE for toolkit compatibility, and
   *          niri has no KScreen. Evaluated per call, never cached.
   */
  bool
  session_supports_kscreen();

  /**
   * @brief Whether a probe may be attempted now (cooldown not armed).
   */
  bool
  probe_allowed();

  /**
   * @brief Record an unavailable backend: arm the cooldown and log once.
   */
  void
  note_unavailable(const std::string &reason);

  /**
   * @brief Record a successful query: clear the cooldown.
   */
  void
  note_available();

  /**
   * @brief Run `kscreen-doctor <args>` with ANSI escapes stripped.
   * @param timeout Bound on the child; a query should use kProbeTimeout.
   */
  exec_result_t
  run(const std::string &args, std::chrono::milliseconds timeout);
}  // namespace platf::kscreen
