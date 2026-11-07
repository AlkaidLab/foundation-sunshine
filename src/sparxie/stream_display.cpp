#include "src/sparxie/stream_display.h"
#include <algorithm>
#include <cctype>

namespace sparxie {
  namespace {
    bool
    unity_title(std::string_view game) { return game == "genshin" || game == "starrail" || game == "hi3" || game == "zzz"; }
    bool
    unreal_title(std::string_view game) { return game == "wuwa" || game == "nte"; }
    // the titles whose game process takes Unity's screen arguments (Endfield is a Unity game too; it is sized through the Sparxie session)
    bool
    screen_args_title(std::string_view game) { return unity_title(game) || game == "endfield"; }
    std::string
    text_of(const json &value, const char *key) {
      return value.is_object() && value.contains(key) && value[key].is_string() ? value[key].get<std::string>() : std::string();
    }
  }  // namespace

  StreamMode
  resolve_stream_mode(const SessionMode &session, std::pair<int, int> display_size, int display_refresh) {
    StreamMode mode;
    if (display_size.first > 0 && display_size.second > 0) {
      mode.width = display_size.first;
      mode.height = display_size.second;
    }
    else if (session.width > 0 && session.height > 0) {
      mode.width = session.width;
      mode.height = session.height;
    }
    mode.refresh = display_refresh > 0 ? display_refresh : session.fps > 0 ? session.fps :
                                                                             0;
    return mode;
  }

  json
  default_display() { return json { { "mode", "follow" } }; }

  json
  normalized_display(const json &value) {
    if (!value.is_object() || text_of(value, "mode") != "fixed") return default_display();
    json out = { { "mode", "fixed" } };
    int width = 0, height = 0;
    const std::string resolution = text_of(value, "resolution");
    if (parse_resolution(resolution, width, height) && width > 0 && height > 0) out["resolution"] = resolution_text(width, height);
    const std::string window = text_of(value, "window");
    out["window"] = window == "borderless" || window == "windowed" ? window : "fullscreen";
    const std::string index = text_of(value, "resolutionIndex");
    if (!index.empty() && std::all_of(index.begin(), index.end(), [](char c) { return c >= '0' && c <= '9'; }) && index.size() < 4) out["resolutionIndex"] = index;
    return out;
  }

  DisplayChoice
  display_choice_of(const json &block) {
    DisplayChoice choice;
    const json display = normalized_display(block.is_object() && block.contains("display") ? block["display"] : json());
    choice.follow = text_of(display, "mode") != "fixed";
    if (choice.follow) return choice;
    choice.resolution = text_of(display, "resolution");
    choice.window = text_of(display, "window");
    const std::string index = text_of(display, "resolutionIndex");
    if (!index.empty()) choice.resolution_index = std::stoi(index);
    return choice;
  }

  void
  mirror_display_to_options(json &block) {
    if (!block.is_object() || block.value("game", std::string()) != "endfield") return;
    if (!block.contains("options") || !block["options"].is_object()) block["options"] = json::object();
    const DisplayChoice choice = display_choice_of(block);
    block["options"]["resolution"] = choice.follow || choice.resolution.empty() ? std::string("auto") : choice.resolution;
  }

  DisplayPlan
  plan_display(std::string_view game, const DisplayChoice &choice, const StreamMode &stream) {
    DisplayPlan plan;
    plan.writes = unity_title(game) || unreal_title(game);
    if (!plan.writes) return plan;
    if (choice.follow) {
      // fullscreen is Unity's full-screen window and Unreal's windowed fullscreen: the title covers the streamed display, which the capture takes, and no exclusive mode takes it away
      plan.window = "fullscreen";
      if (stream.known()) plan.resolution = resolution_text(stream.width, stream.height);
      if (game == "zzz") plan.resolution_index = 0;
      return plan;
    }
    plan.window = choice.window;
    // A Unity title's borderless window is a framed-less window (-popupwindow), not full screen: the registry says windowed and the launch arguments add -popupwindow.
    if (unity_title(game) && plan.window == "borderless") plan.window = "windowed";
    plan.resolution = choice.resolution;
    if (game == "zzz") plan.resolution_index = choice.resolution_index;
    return plan;
  }

  ScreenPlan
  plan_screen(std::string_view game, const DisplayChoice &choice, const StreamMode &stream) {
    ScreenPlan plan;
    if (!screen_args_title(game)) return plan;
    int width = 0, height = 0;
    if (choice.follow) {
      plan.window = "fullscreen";
      if (stream.known()) {
        width = stream.width;
        height = stream.height;
      }
    }
    else {
      plan.window = choice.window == "windowed" || choice.window == "borderless" ? choice.window : "fullscreen";
      if (!parse_resolution(choice.resolution, width, height)) width = height = 0;
    }
    if (width >= 320 && width <= 16384 && height >= 320 && height <= 16384) {
      plan.width = width;
      plan.height = height;
    }
    return plan;
  }

  std::string
  unity_screen_arguments(const ScreenPlan &plan) {
    std::string out;
    if (plan.width > 0) out += " -screen-width " + std::to_string(plan.width) + " -screen-height " + std::to_string(plan.height);
    if (plan.window == "fullscreen") out += " -screen-fullscreen 1";
    else if (plan.window == "windowed") out += " -screen-fullscreen 0";
    else if (plan.window == "borderless") out += " -screen-fullscreen 0 -popupwindow";
    return out;
  }

  std::string
  launcher_screen_arguments(const ScreenPlan &plan) {
    std::string out;
    if (plan.width > 0) out += " --screen-width " + std::to_string(plan.width) + " --screen-height " + std::to_string(plan.height);
    if (!plan.window.empty()) out += " --window-mode " + plan.window;
    return out;
  }

  std::string
  stream_launch_arguments(std::string_view game, std::string_view command, const DisplayChoice &choice, const StreamMode &stream, bool through_launcher) {
    const ScreenPlan plan = plan_screen(game, choice, stream);
    // following the stream without knowing its size sends nothing: the registry write (full screen) is all there is
    if (plan.empty() || (choice.follow && plan.width == 0)) return {};
    if (through_launcher) return command.find("--screen-width") != std::string_view::npos ? std::string() : launcher_screen_arguments(plan);
    // the shipped launcher's Endfield direct start (decoy parent): the launcher puts what follows its `--` separator on the game's command line
    // (it accepts only these integers and fixed tokens)
    if (command.find("--endfield-direct") != std::string_view::npos) {
      if (game != "endfield" || command.find(" -- ") != std::string_view::npos) return {};
      return " --" + unity_screen_arguments(plan);
    }
    // only the game's own executable takes Unity's arguments
    if (command.find("://") != std::string_view::npos) return {};
    std::string_view tail = command;
    while (!tail.empty() && (tail.back() == ' ' || tail.back() == '"')) tail.remove_suffix(1);
    if (tail.size() < 4) return {};
    std::string end(tail.substr(tail.size() - 4));
    for (char &c : end) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (end != ".exe" || command.find("-screen-width") != std::string_view::npos) return {};
    return unity_screen_arguments(plan);
  }
}  // namespace sparxie
