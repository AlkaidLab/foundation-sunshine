#pragma once

#include "src/sparxie/model.h"
#include <string>
#include <string_view>
#include <utility>

namespace sparxie {
  // The display of a game app: 跟随串流 (the default) or 固定.
  //
  // 跟随串流: at every launch the game runs fullscreen at the size of the display that is being streamed (the Foundation virtual display, or
  // the physical one), so a phone or a tablet gets a picture that fills the screen, with no letterbox and no window frame.
  // 固定: the person picks the size and the window mode.
  //
  // It is stored in the application's sparxie block as { "mode": "follow" | "fixed", "resolution": "1920×1080", "window": "fullscreen" |
  // "borderless" | "windowed", "resolutionIndex": "3" } with every value a string (the generic apps writer keeps nothing else).
  // The size is not stored for 跟随串流: the host asks the session at launch.

  // The mode the client asked for in this session (0 when the host was not told).
  struct SessionMode {
    int width = 0;
    int height = 0;
    int fps = 0;
  };

  // The mode the stream really has, from the display being streamed and from the session.
  struct StreamMode {
    int width = 0;
    int height = 0;
    int refresh = 0;
    bool known() const { return width > 0 && height > 0; }
  };

  // The display itself wins: the host has already configured it for this session when the app starts, and it is what the game covers.
  // The session's own numbers fill in what the display could not tell.
  StreamMode
  resolve_stream_mode(const SessionMode &session, std::pair<int, int> display_size, int display_refresh);

  struct DisplayChoice {
    bool follow = true;
    std::string resolution;  // fixed: "1920×1080"
    std::string window = "fullscreen";  // fixed: fullscreen | borderless | windowed
    int resolution_index = -1;  // fixed, Zenless Zone Zero: a position in the game's own list
  };

  // The stored choice of a sparxie block; an application without one follows the stream.
  DisplayChoice
  display_choice_of(const json &block);

  // A valid stored object for a request's value ("follow" when it is missing or unreadable).
  json
  normalized_display(const json &value);

  // The default of an application that never chose: 跟随串流.
  json
  default_display();

  // An Endfield profile keeps its size in options.resolution ("auto" follows the stream); the choice is mirrored there.
  void
  mirror_display_to_options(json &block);

  // What the host writes into the game's own settings at launch.
  struct DisplayPlan {
    // "1920×1080"; empty when the size is not known (the game's stored size stays)
    std::string resolution;
    // fullscreen | borderless | windowed, as write_game_settings takes it
    std::string window;
    // Zenless Zone Zero stores its size as a position in a list; 0 is the screen's own size
    int resolution_index = -1;
    // the title has no settings this host writes (Endfield is sized by the Sparxie session, the other titles keep their own)
    bool writes = false;
  };
  DisplayPlan
  plan_display(std::string_view game, const DisplayChoice &choice, const StreamMode &stream);

  // The size and window mode a launch asks the game for, whichever way the game starts: the stream's size in full screen (跟随串流), or the person's size and window mode (固定).
  // These are the same numbers the host writes into the game's settings, so the registry and the command line agree.
  struct ScreenPlan {
    int width = 0;  // 0 = no size (the stream's size is not known, or the title takes none)
    int height = 0;
    std::string window;  // fullscreen | windowed | borderless; empty = none
    bool empty() const { return width == 0 && window.empty(); }
  };
  ScreenPlan
  plan_screen(std::string_view game, const DisplayChoice &choice, const StreamMode &stream);

  // Unity's own arguments (Unity manual, Standalone Player command-line arguments; the community launchers Collapse and Starward pass the same ones), with a leading space:
  // " -screen-width W -screen-height H -screen-fullscreen 0|1 [-popupwindow]". Empty for an empty plan.
  std::string
  unity_screen_arguments(const ScreenPlan &plan);

  // The arguments of sparxie-launch.exe that carry the plan into the Sparxie session: " --screen-width W --screen-height H --window-mode M".
  std::string
  launcher_screen_arguments(const ScreenPlan &plan);

  // What to append to an application's command so the game gets the plan. `through_launcher` is a touch application (the command is sparxie-launch.exe, which hands the plan to
  // Sparxie); otherwise the command is the game's own executable and the Unity arguments are appended. Empty for a command that is not one of those (a store address, an official
  // launcher), for a title that takes no Unity arguments, and for a command that already has a size.
  std::string
  stream_launch_arguments(std::string_view game, std::string_view command, const DisplayChoice &choice, const StreamMode &stream, bool through_launcher = false);
}  // namespace sparxie
