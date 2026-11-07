/**
 * @file src/launch_splash/launch_splash.h
 * @brief The host side of the launch splash: starts and ends the state, reads the launcher's status file and publishes it in the tray state.
 *
 * The Panel (sunshine-gui, in the user's session) draws the splash and finds the game's window; the host cannot, because Core may run as a
 * service in session 0. The rules are in launch_splash_model.h; the contract with launchers and the Panel is docs/launch-splash.md.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "launch_splash_model.h"
#include "src/process.h"

namespace launch_splash {

  /// What a launcher is told, as environment variables of the app command.
  struct launcher_env_t {
    std::string id;  ///< SUNSHINE_LAUNCH_ID
    std::filesystem::path status_file;  ///< SUNSHINE_LAUNCH_STATUS_FILE, a file the launcher may write; it is removed when the launch ends
  };

  /// Whether a command is a launcher that reports its own stages (sparxie-launch).
  bool
  command_reports_stages(const std::string &cmd);

  /**
   * A launch request for an app with a command was accepted, before the display and encoder setup. The Panel is told at once (display_ready false) so it
   * can make its window ready while the setup runs. False when the setting 启动画面 is off or the app has no command. begin() takes the announced launch over.
   */
  bool
  announce(const proc::ctx_t &app);

  /// The display the stream captures is set up: the Panel may show the splash on it now.
  void
  display_ready();

  /// The request failed before the app started: an announced launch that begin() has not taken over goes.
  void
  cancel_announced();

  /**
   * The Panel reports (tray action launch_splash_shown) that the splash of launch `id` is on the display with its first frame painted.
   * `panel_ms` is what the Panel measured from receiving "display ready" to that frame; `prepare_ms` from receiving the announcement to its window being ready.
   */
  bool
  note_shown(const std::string &id, std::int64_t panel_ms, std::int64_t prepare_ms);

  /// Just before the app command starts: waits until the Panel has the splash on screen, at most shown_wait_ms, and logs the measured time.
  /// Nothing to wait for (no splash, a resume, no Panel that can answer): returns at once.
  void
  wait_shown();

  /**
   * An app with a command is about to start. Does nothing (nullopt) when the setting 启动画面 is off. Otherwise the splash state is published
   * at stage starting and the launcher's environment is returned.
   */
  std::optional<launcher_env_t>
  begin(const proc::ctx_t &app, kind_e kind);

  /// The app's process exists: its id goes to the Panel's window search. For a command that does not report stages the stage becomes waiting_window.
  void
  note_started(std::uint32_t pid, bool launcher_reports);

  /// The launch did not work; the reason is a short code (start: the command could not run; prep: a prepare command failed).
  void
  fail(const std::string &reason);

  /// The app or the stream ended. A failure that is being read stays until its time is up.
  void
  end();

}  // namespace launch_splash
