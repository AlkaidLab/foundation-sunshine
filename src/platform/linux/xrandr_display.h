#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

/**
 * @file src/platform/linux/xrandr_display.h
 * @brief Display configuration through the X11 RandR extension.
 *
 * KScreen, Mutter's D-Bus interface and niri's IPC all describe a Wayland-ish
 * model. A bare X11 session (i3, openbox, bspwm, an X11 desktop session that
 * exposes neither kscreen-doctor nor Mutter) has none of them, and RandR is the
 * only thing there is. Its model differs enough from the other three that the
 * differences are worth stating, because they decide how this header is shaped:
 *
 *  - An *output* is a connector; a *crtc* is a timing generator that scans one
 *    mode onto one or more outputs. Two outputs on one crtc is exactly what
 *    mirroring is, and it requires them to share the mode.
 *  - Disabling an output means leaving it off a crtc: there is no "off" verb.
 *    Setting a crtc's output list to empty is the way to switch it off.
 *  - A write is a sequence of `XRRSetCrtcConfig` calls plus one screen size,
 *    not a transaction. RandR has no serial and no verification, so a rejected
 *    request leaves a partially applied configuration behind: apply() therefore
 *    snapshots every crtc first and rolls back on the first error.
 *  - RandR knows nothing about HDR, colour modes, scaling or fractional refresh
 *    rates: the refresh is whatever the mode's timings work out to. There is
 *    deliberately no HDR setter here, and the state reports no HDR capability.
 *  - Primary is a per-screen property of one output.
 *
 * The pure planner below (`plan_for`) is split out on purpose: it decides which
 * output goes on which crtc, at which position, with which mode, and that is the
 * part worth testing without an X server. Everything that touches Xlib is in
 * the implementation and stays thin.
 */
namespace platf::xrandr {

  /// A mode as RandR reports it. There is no server-side id to hand back, so
  /// the id is our own formatting of size and refresh.
  struct mode_t {
    std::string id;
    int width = 0;
    int height = 0;
    double refresh = 0.0;
    bool current = false;
    bool preferred = false;
  };

  struct output_t {
    std::string name;
    bool connected = false;
    bool enabled = false;
    bool primary = false;

    /// Top-left corner and size of the rectangle the output scans.
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    /// Set while the output is enabled.
    std::string mode_id;
    double refresh = 0.0;

    /// Quarter turns counter-clockwise, 0..3, and the two reflections.
    int rotation = 0;
    bool reflect_x = false;
    bool reflect_y = false;

    std::vector<mode_t> modes;

    /**
     * @brief Name of the output this one mirrors, or empty.
     * @details Set for every member of a shared crtc except the first one, so
     *          the group can be rebuilt from any of its members.
     */
    std::string mirror_of;
  };

  struct state_t {
    int screen_width = 0;
    int screen_height = 0;
    std::vector<output_t> outputs;
  };

  /**
   * @brief Whether this session can be driven through RandR.
   * @details Requires a reachable X display that offers RandR 1.2 or newer and
   *          can enumerate its screen resources. Wayland sessions are *not*
   *          excluded on purpose: XWayland answers reads, and the write path
   *          fails cleanly there, so the probe reports what the server says and
   *          leaves the verdict to the caller that actually writes.
   */
  bool
  session_supports_xrandr();

  /// False while a recent failure is still cooling down.
  bool
  probe_allowed();

  /// Record that the server answered; clears the cooldown.
  void
  note_available();

  /// Record why the server was unusable; starts the cooldown.
  void
  note_unavailable(const std::string &reason);

  /**
   * @brief Read the current configuration.
   * @return The state, or nothing when there is no usable X display.
   */
  std::optional<state_t>
  query_state();

  /**
   * @brief Make the server match @p state.
   * @details Reads the live configuration, plans the crtc assignments, applies
   *          them under a server grab and rolls the snapshot back when any
   *          request is rejected. @p error receives the first failure.
   */
  bool
  apply(const state_t &state, std::string &error);

  /// Output entry for @p name, or nullptr.
  const output_t *
  find_output(const state_t &state, const std::string &name);

  /// Output entry for @p name, mutable, or nullptr.
  output_t *
  find_output(state_t &state, const std::string &name);

  /// The mode @p output currently scans, or nullptr when it is disabled.
  const mode_t *
  current_mode(const output_t &output);

  /// The mode of @p output matching the given size and rate, or nullptr.
  const mode_t *
  matching_mode(const output_t &output, int width, int height, double refresh);

  // ── exposed for tests ────────────────────────────────────────────────────
  /**
   * @brief One crtc's worth of assignments: the outputs it drives and where.
   * @details RandR applies a mode per crtc, so every member of a mirror group
   *          must share it; the planner enforces that instead of letting the
   *          server pick one silently.
   */
  struct crtc_request_t {
    std::vector<std::string> outputs;
    std::string mode_id;
    int width = 0;
    int height = 0;
    int x = 0;
    int y = 0;
    int rotation = 0;
    bool reflect_x = false;
    bool reflect_y = false;
  };

  struct plan_t {
    int screen_width = 0;
    int screen_height = 0;
    std::vector<crtc_request_t> crtcs;
    /// The output that should become the screen's primary, or empty.
    std::string primary;
  };

  /**
   * @brief Turn a desired state into crtc assignments.
   * @details Pure: it looks only at @p desired, and it fails instead of guessing
   *          when the state cannot be expressed (an unknown output or mode, a
   *          mirror group whose members disagree on the mode).
   */
  bool
  plan_for(const state_t &desired, plan_t &out, std::string &error);

  /// The refresh a mode's timings work out to, from its dot clock.
  double
  refresh_from_timings(std::uint64_t dot_clock, int htotal, int vtotal);

}  // namespace platf::xrandr
