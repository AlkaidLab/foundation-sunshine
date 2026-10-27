/**
 * @file src/platform/linux/niri_ipc.h
 * @brief When to talk to niri's IPC, and how to run one bounded request.
 *
 * niri is the second target session. Like kscreen-doctor it exposes output
 * management over a tool rather than D-Bus (`niri msg`), but the similarities
 * end at the CLI shape: the tool authenticates over a Unix socket that niri
 * hands out as NIRI_SOCKET, accepts one request per invocation, and answers
 * with JSON on stdout.
 *
 * Two rules, the same ones kscreen needs:
 *   - a session that cannot have niri is detected from the environment and
 *     never probed (re-checked per call: the service may start before the
 *     desktop, and must pick it up once it appears), and
 *   - a probe that fails arms a cooldown, so a stale socket costs one short
 *     probe per window instead of one failed process per query.
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace platf::niri {
  /// A read must answer within this window; `niri msg` either talks to a
  /// running compositor or fails, so waiting longer only delays the session.
  inline constexpr auto kProbeTimeout = std::chrono::milliseconds { 1500 };

  /// A change waits for the compositor to apply it, so it gets a wider bound
  /// than a read. The CLI itself returns immediately (it only reports whether
  /// the request was handed over), so callers must verify by re-reading.
  inline constexpr auto kChangeTimeout = std::chrono::milliseconds { 10'000 };

  /// After a failed probe, only retry once per this window.
  inline constexpr auto kRetryCooldown = std::chrono::seconds { 20 };

  struct exec_result_t {
    int exit_code { -1 };
    std::string output;
  };

  /// One mode as `niri msg --json outputs` reports it. niri has no mode ids and
  /// no mode names, so a mode is identified by its geometry alone.
  struct mode_t {
    unsigned int width { 0 };
    unsigned int height { 0 };
    /// Rate in Hz, converted from the millihertz niri reports.
    double refresh { 0.0 };
    bool preferred { false };
  };

  /// One output as niri reports it, before the display_device mapping.
  struct output_t {
    std::string name;
    std::vector<mode_t> modes;
    /// Index into `modes` of the active mode; unset when the output is off.
    std::optional<std::size_t> current_mode;
  };

  /**
   * @brief Parse a `niri msg --json outputs` payload.
   *
   * Kept separate from the process plumbing so the wire shape can be tested
   * without a compositor. Returns an empty list and fills `error` (when given)
   * on a payload that is not the expected object.
   */
  std::vector<output_t>
  parse_outputs(std::string_view json, std::string *error = nullptr);

  /**
   * @brief Whether this session can be a niri session at all.
   * @details niri exports NIRI_SOCKET to its children and, through the user
   *          manager, to services started from that session. Setups that fake
   *          XDG_CURRENT_DESKTOP=KDE for toolkit compatibility still export it,
   *          so it wins over the desktop string. When neither is conclusive a
   *          socket under the user's runtime directory is looked for, which is
   *          how a service started at boot finds the desktop it missed.
   *          Evaluated per call, never cached; the socket search itself is
   *          bounded by a cooldown.
   */
  bool
  session_supports_niri();

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
   * @brief Record a successful request: clear the cooldown.
   */
  void
  note_available();

  /**
   * @brief Run `niri msg <args>` against the session's IPC socket.
   * @param timeout Bound on the child; a read should use kProbeTimeout.
   */
  exec_result_t
  run(const std::string &args, std::chrono::milliseconds timeout);

  /**
   * @brief The IPC socket in use, empty when the session has none.
   * @details Exposed for diagnostics: the socket name carries the compositor
   *          instance, which explains a stale-socket failure to the user.
   */
  std::string
  socket_path();
}  // namespace platf::niri
