/**
 * @file src/launch_splash/launch_splash_model.h
 * @brief What the launch splash shows and when it goes: the stage vocabulary, the launcher's status file and the lifetime rules.
 *
 * The splash is drawn by the Panel in the user's session; the host only decides the state (stage, one notice line, the process ids to
 * look for, a deadline) and publishes it in the tray state. This file is pure: no clock, no file, no log of its own, so the rules are
 * testable on any platform.
 *
 * The state ends, whichever comes first: the Panel sees the game's window and stops showing it (it needs no word from the host), the
 * host calls end() (the app or the stream ended), or the deadline passes (timeout_ms after the start; a failure shortens it to
 * notice_ms so the reason can be read).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace launch_splash {

  /// No game window by then: the splash goes (60 s).
  inline constexpr std::int64_t timeout_ms = 60'000;
  /// A resumed app already has its window; if the Panel cannot find it, the splash is not left over the game for long.
  inline constexpr std::int64_t resume_timeout_ms = 8'000;
  /// The most the app start waits for the Panel to have the splash on the streamed display (first frame painted). The wait is skipped when no Panel that
  /// can answer is connected, and it normally costs nothing: the display and encoder setup takes seconds, the Panel needs tens of milliseconds.
  inline constexpr std::int64_t shown_wait_ms = 800;
  /// How long a failure or a fallback line stays on the screen.
  inline constexpr std::int64_t notice_ms = 5'000;
  /// More process ids than this are of no use to the window search.
  inline constexpr std::size_t max_pids = 16;

  /**
   * Stages in the order they are reached. A plain application goes starting -> waiting_window; a Sparxie touch application goes through the
   * launcher's own stages. failed is the end of a launch that did not work: it shows its reason and then goes.
   */
  enum class stage_e {
    starting,
    broker,  ///< the launcher starts Sparxie's Broker
    preparing,  ///< Sparxie prepares the session
    injecting,  ///< Sparxie injects its touch layer into the game
    waiting_window,  ///< the game was started; its window is not up yet
    failed,
  };

  enum class kind_e {
    launch,
    resume,  ///< the app already runs: the Panel shows the splash only when the window is still missing a moment after the request
  };

  std::string_view
  stage_key(stage_e stage);

  std::optional<stage_e>
  stage_from_key(std::string_view key);

  std::string_view
  kind_key(kind_e kind);

  /// What a launcher wrote in the status file (src/launch_splash/status file contract in docs/launch-splash.md).
  struct launcher_status_t {
    std::string launch;  ///< the SUNSHINE_LAUNCH_ID the host gave; a file of another launch is not this launcher's
    std::optional<stage_e> stage;
    std::string reason;  ///< a short code for stage failed (broker, rejected, rpc, session-failed, lost, timed-out, config, internal ...)
    std::string notice;  ///< a short code for a line that does not end the launch (direct: Sparxie could not take over, the game was started directly)
    std::vector<std::uint32_t> pids;  ///< the game's own processes, once the launcher knows them
  };

  /// nullopt for text that is not a status object; unknown fields are ignored, a stage it does not know is no stage.
  std::optional<launcher_status_t>
  parse_launcher_status(std::string_view text);

  struct view_t {
    bool active = false;
    std::string id;
    kind_e kind = kind_e::launch;
    std::string app_id;
    std::string app_name;
    std::string art;  ///< a picture file the Panel may read for the backdrop (the hero, else the poster), or empty
    stage_e stage = stage_e::starting;
    std::string notice;  ///< the code of the line to show under the stage, or empty
    /// False from the moment the request is accepted until the display the stream captures is set up: the Panel makes its window ready (page loaded, picture
    /// decoded) but does not show it, because it would land on a display that is about to change.
    bool display_ready = true;
    std::vector<std::uint32_t> pids;
    std::int64_t started_at_ms = 0;
    std::int64_t deadline_ms = 0;
  };

  nlohmann::json
  to_json(const view_t &view);

  class tracker_t {
  public:
    /// Starts a launch (replacing any before it) and returns its id.
    std::string
    begin(std::string app_id, std::string app_name, std::string art, kind_e kind, std::int64_t now_ms, bool display_ready = true);

    /// The display the stream captures is set up. False when nothing was waiting for it.
    bool
    set_display_ready();

    void
    add_pid(std::uint32_t pid);

    /// Takes a launcher's status. False when nothing changed: the file is of another launch, the stage does not advance, or the launch is over.
    bool
    apply(const launcher_status_t &status, std::int64_t now_ms);

    /// The launch did not work: the stage becomes failed with this reason, and the state goes notice_ms later.
    bool
    fail(std::string reason, std::int64_t now_ms);

    /// The app or the stream ended. False when nothing was showing.
    bool
    end();

    /// Lets the clock act: a timeout ends the launch, an old notice line is dropped. True when the state changed.
    bool
    tick(std::int64_t now_ms);

    const view_t &
    view() const {
      return view_;
    }

    const std::string &
    id() const {
      return view_.id;
    }

  private:
    view_t view_;
    std::int64_t notice_until_ms_ = 0;
    std::uint64_t counter_ = 0;
  };

}  // namespace launch_splash
