/**
 * @file src/platform/linux/compositor_output.h
 * @brief Which compositor command lights up a freshly created output.
 *
 * The DRM-level CRTC assignment is what actually brings a virtual display up;
 * this is only the compositor-side nudge that makes it paint. Sunshine targets
 * KDE Plasma and niri, and prefers a generic mechanism wherever one exists, so
 * the desktop is mapped onto the tool that owns output control there:
 *
 *   KDE Plasma - kscreen-doctor (the historically tested path here)
 *   niri       - `niri msg output <name> on` over niri's own IPC
 *   other      - wlr-randr (wlr-output-management: sway/Hyprland/river/...)
 *   X11        - xrandr
 *   unknown    - nothing: compositors normally enable a new output by
 *                themselves, so an empty result must not be a failure
 *
 * enable_command() is a pure mapping (every probe is a parameter) so the
 * dispatch is unit-tested without a compositor. Callers that need the real
 * probes use niri_session()/tool_available() below.
 */
#pragma once

#include <string>

namespace platf::compositor_output {
  /**
   * @brief Whether this process runs inside a niri session.
   * @details niri exports NIRI_SOCKET to every child, so its presence is the
   *          cheapest reliable discriminator (plain getenv: the AT_SECURE
   *          file capabilities only affect libc's secure_getenv users).
   */
  bool
  niri_session();

  /**
   * @brief Whether an executable exists in the usual bin directories.
   * @details Used to pick a compositor control tool without spawning a shell
   *          probe per call; a wrong guess only costs one failed command.
   */
  bool
  tool_available(const char *name);

  /**
   * @brief The command that asks @p desktop to enable @p connector.
   * @return The command line, or an empty string when the session has no known
   *         output control tool.
   * @details KDE is deliberately not gated on tool_available("kscreen-doctor"):
   *          that is the tested path, and a missing tool only costs one failed
   *          command (run_logged already bounds it).
   */
  std::string
  enable_command(const std::string &desktop,
                 const std::string &connector,
                 bool niri,
                 bool wlr_randr,
                 bool x11_display,
                 bool xrandr);
}  // namespace platf::compositor_output
