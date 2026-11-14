#include "src/sparxie/host.h"
#include "src/sparxie/stream_display.h"
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <thread>

namespace fs = std::filesystem;
using sparxie::json;

// 跟随串流 / 固定: the display choice of a game app, what the host writes into each title at launch, the arguments of a direct start, the migration.
namespace {
  fs::path
  fresh(const char *name) {
    const fs::path root = fs::temp_directory_path() / name;
    fs::remove_all(root);
    fs::create_directories(root);
    return root;
  }
  sparxie::HostEnv
  env_at(const fs::path &root) {
    sparxie::HostEnv env;
    env.host_data = root / "host";
    env.data_dir = root / "sparxie-data";
    env.apps_file = root / "apps.json";
    env.covers_dir = root / "covers";
    env.install_root = root / "install";
    env.component_dir = root / "components" / "sparxie";
    return env;
  }
  void
  touch(const fs::path &path) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << "x";
  }
  void
  write_apps(const sparxie::HostEnv &env, const json &app) {
    std::ofstream out(env.apps_file);
    out << json { { "apps", { app } } }.dump();
  }
  json
  block_of(const fs::path &root, const char *game, const char *region, const char *exe, const char *mode, const json &extra = json::object()) {
    json block = { { "install", std::string(game) + "-" + region }, { "game", game }, { "region", region }, { "dir", (root / "game").string() }, { "exe", (root / "game" / exe).string() }, { "mode", mode }, { "restore", "restore" } };
    for (auto it = extra.begin(); it != extra.end(); ++it) block[it.key()] = it.value();
    return block;
  }
  std::string
  key_of(const char *vendor, const char *name) {
    return std::string("Software") + static_cast<char>(92) + vendor + static_cast<char>(92) + name;
  }
  sparxie::GameValues
  read_back(const sparxie::Install &install, sparxie::Registry &reg) {
    return sparxie::read_game_settings(install, reg, {}, [] { return false; }).values;
  }
  sparxie::Install
  install_of(const json &block) {
    sparxie::Install install;
    install.id = block.value("install", "");
    install.game = block.value("game", "");
    install.region = block.value("region", "");
    install.dir = block.value("dir", "");
    install.exe_path = block.value("exe", "");
    return install;
  }
}  // namespace

TEST(StreamDisplay, TheDisplayTheHostStreamsWinsAndTheSessionFillsIn) {
  const auto both = sparxie::resolve_stream_mode({ 1920, 1080, 60 }, { 2868, 1320 }, 120);
  EXPECT_EQ(both.width, 2868);
  EXPECT_EQ(both.height, 1320);
  EXPECT_EQ(both.refresh, 120);
  const auto session_only = sparxie::resolve_stream_mode({ 2560, 1600, 90 }, { 0, 0 }, 0);
  EXPECT_EQ(session_only.width, 2560);
  EXPECT_EQ(session_only.height, 1600);
  EXPECT_EQ(session_only.refresh, 90);
  const auto none = sparxie::resolve_stream_mode({}, { 0, 0 }, 0);
  EXPECT_FALSE(none.known());
  EXPECT_EQ(none.refresh, 0);
}

TEST(StreamDisplay, ChoiceIsFollowUnlessFixedAndIsCleanedUp) {
  EXPECT_TRUE(sparxie::display_choice_of(json::object()).follow);
  EXPECT_TRUE(sparxie::display_choice_of({ { "display", { { "mode", "nonsense" } } } }).follow);
  EXPECT_EQ(sparxie::normalized_display(json()), sparxie::default_display());
  const auto fixed = sparxie::display_choice_of({ { "display", { { "mode", "fixed" }, { "resolution", "2560x1440" }, { "window", "borderless" } } } });
  EXPECT_FALSE(fixed.follow);
  EXPECT_EQ(fixed.resolution, "2560×1440");
  EXPECT_EQ(fixed.window, "borderless");
  const json odd = sparxie::normalized_display({ { "mode", "fixed" }, { "resolution", "wide" }, { "window", "exclusive" }, { "resolutionIndex", "x1" } });
  EXPECT_EQ(odd, (json { { "mode", "fixed" }, { "window", "fullscreen" } })) << "unusable values are dropped, the window mode falls back to fullscreen";
  EXPECT_EQ(sparxie::display_choice_of({ { "display", { { "mode", "fixed" }, { "resolutionIndex", "4" } } } }).resolution_index, 4);
}

TEST(StreamDisplay, PlanPerTitle) {
  const sparxie::StreamMode phone { 2868, 1320, 120 };
  const sparxie::DisplayChoice follow;
  for (const char *game : { "genshin", "starrail", "hi3", "wuwa", "nte" }) {
    const auto plan = sparxie::plan_display(game, follow, phone);
    EXPECT_TRUE(plan.writes) << game;
    EXPECT_EQ(plan.resolution, "2868×1320") << game;
    EXPECT_EQ(plan.window, "fullscreen") << game << ": the full-screen window, never exclusive, never a window";
    EXPECT_EQ(plan.resolution_index, -1) << game;
  }
  const auto zzz = sparxie::plan_display("zzz", follow, phone);
  EXPECT_EQ(zzz.resolution_index, 0) << "Zenless Zone Zero: position 0 is the screen's own size";
  for (const char *game : { "endfield", "arknights", "pgr" }) EXPECT_FALSE(sparxie::plan_display(game, follow, phone).writes) << game << " is sized by its own session or keeps its settings";
  const auto unknown = sparxie::plan_display("starrail", follow, {});
  EXPECT_TRUE(unknown.resolution.empty()) << "an unknown stream size leaves the stored size alone";
  EXPECT_EQ(unknown.window, "fullscreen") << "but the window is never the in-between one";
  sparxie::DisplayChoice fixed;
  fixed.follow = false;
  fixed.resolution = "1920×1080";
  fixed.window = "windowed";
  const auto windowed = sparxie::plan_display("starrail", fixed, phone);
  EXPECT_EQ(windowed.resolution, "1920×1080");
  EXPECT_EQ(windowed.window, "windowed");
  fixed.window = "borderless";
  EXPECT_EQ(sparxie::plan_display("genshin", fixed, phone).window, "windowed") << "a Unity title's borderless window is a frame-less window: the registry says windowed and -popupwindow does the rest";
  EXPECT_EQ(sparxie::plan_display("wuwa", fixed, phone).window, "borderless");
}

TEST(StreamDisplay, DirectStartsGetUnitySizeArguments) {
  const sparxie::StreamMode phone { 2868, 1320, 120 };
  const sparxie::DisplayChoice follow;
  EXPECT_EQ(sparxie::stream_launch_arguments("starrail", "\"C:\\Games\\StarRail\\StarRail.exe\"", follow, phone), " -screen-width 2868 -screen-height 1320 -screen-fullscreen 1");
  EXPECT_EQ(sparxie::stream_launch_arguments("genshin", "C:\\Games\\Genshin\\YuanShen.EXE", follow, phone), " -screen-width 2868 -screen-height 1320 -screen-fullscreen 1");
  EXPECT_EQ(sparxie::stream_launch_arguments("zzz", "C:\\Z\\ZenlessZoneZero.exe", follow, phone), " -screen-width 2868 -screen-height 1320 -screen-fullscreen 1") << "Collapse passes the size arguments to Zenless Zone Zero too; its list position in the registry agrees";
  EXPECT_TRUE(sparxie::stream_launch_arguments("genshin", "steam://rungameid/1", follow, phone).empty()) << "a store address takes no arguments";
  EXPECT_TRUE(sparxie::stream_launch_arguments("genshin", "\"C:\\Sparxie\\sparxie-launch.exe\" --profile x --sparxie y.json", follow, phone).empty()) << "a launcher command is not the game";
  EXPECT_TRUE(sparxie::stream_launch_arguments("wuwa", "C:\\W\\Wuthering Waves.exe", follow, phone).empty()) << "Unreal titles use their file";
  EXPECT_TRUE(sparxie::stream_launch_arguments("starrail", "C:\\S\\StarRail.exe", follow, {}).empty());
  EXPECT_TRUE(sparxie::stream_launch_arguments("starrail", "C:\\S\\StarRail.exe -screen-width 1280", follow, phone).empty());
  sparxie::DisplayChoice fixed;
  fixed.follow = false;
  fixed.resolution = "1280×720";
  fixed.window = "windowed";
  EXPECT_EQ(sparxie::stream_launch_arguments("starrail", "C:\\S\\StarRail.exe", fixed, phone), " -screen-width 1280 -screen-height 720 -screen-fullscreen 0") << "a fixed display takes the same rule";
  EXPECT_TRUE(sparxie::stream_launch_arguments("starrail", "C:\\S\\StarRail.exe -screen-width 1280 -screen-height 720", fixed, phone).empty()) << "a size that is in the stored command is not sent twice";
}

TEST(StreamDisplay, ApplicationsWithoutAChoiceFollowTheStreamAndAnEndfieldSizeStaysFixed) {
  json list = json::array();
  list.push_back({ { "name", "a" }, { "sparxie", { { "game", "starrail" }, { "settings", { { "resolution", "1920×1080" }, { "display", "fullscreen" } } } } } });
  list.push_back({ { "name", "e1" }, { "sparxie", { { "game", "endfield" }, { "options", { { "resolution", "1920×1080" } } } } } });
  list.push_back({ { "name", "e2" }, { "sparxie", { { "game", "endfield" }, { "options", { { "resolution", "auto" } } } } } });
  list.push_back({ { "name", "plain" }, { "cmd", "x" } });
  json apps = json::object();
  apps["apps"] = list;
  EXPECT_TRUE(sparxie::normalize_apps(apps));
  EXPECT_EQ(apps["apps"][0]["sparxie"]["display"], sparxie::default_display()) << "the seeded 1920×1080 of the first builds was not a choice";
  EXPECT_EQ(apps["apps"][1]["sparxie"]["display"], (json { { "mode", "fixed" }, { "resolution", "1920×1080" }, { "window", "fullscreen" } })) << "an Endfield size was a choice";
  EXPECT_EQ(apps["apps"][2]["sparxie"]["display"], sparxie::default_display());
  EXPECT_EQ(apps["apps"][2]["sparxie"]["options"]["resolution"], "auto");
  EXPECT_EQ(apps["apps"][1]["sparxie"]["options"]["resolution"], "1920×1080");
  EXPECT_FALSE(apps["apps"][3].contains("sparxie"));
  EXPECT_FALSE(sparxie::normalize_apps(apps)) << "once normalized, nothing changes again";
  apps["apps"][1]["sparxie"]["display"] = sparxie::default_display();
  EXPECT_TRUE(sparxie::normalize_apps(apps));
  EXPECT_EQ(apps["apps"][1]["sparxie"]["options"]["resolution"], "auto") << "the Endfield session size follows the display choice";
}

TEST(StreamDisplay, LaunchWritesTheStreamedSizeIntoEachUnityTitleWhateverTheSettingsMode) {
  const fs::path root = fresh("sparxie-sd-unity");
  sparxie::HostEnv env = env_at(root);
  sparxie::MemoryRegistry reg;
  env.registry = &reg;
  env.display_size = [] { return std::pair<int, int> { 2868, 1320 }; };
  env.display_refresh = [] { return 120; };
  struct Row {
    const char *game, *region, *exe;
  };
  for (const Row &row : { Row { "genshin", "cn", "YuanShen.exe" }, Row { "starrail", "cn", "StarRail.exe" }, Row { "hi3", "cn", "BH3.exe" }, Row { "zzz", "cn", "ZenlessZoneZero.exe" } }) {
    touch(root / "game" / row.exe);
    for (const char *settings_mode : { "auto", "preset" }) {
      json block = block_of(root, row.game, row.region, row.exe, "pc", { { "settings", settings_mode == std::string("preset") ? json { { "resolution", "1280×720" }, { "display", "windowed" } } : json::object() } });
      write_apps(env, { { "name", "G" }, { "cmd", "x" }, { "sparxie", block } });
      // the session the client asked for is smaller: the display that is streamed wins
      ASSERT_TRUE(sparxie::prepare_launch(env, "G", "x", { 1920, 1080, 60 }).ok) << row.game;
      const auto values = read_back(install_of(block), reg);
      if (std::string(row.game) != "zzz") EXPECT_EQ(values.resolution, "2868×1320") << row.game << " " << settings_mode;
      EXPECT_EQ(values.display, "fullscreen") << row.game << " " << settings_mode;
    }
  }
  // Star Rail keeps its size and mode in a JSON object too
  const auto blob = reg.get(key_of("miHoYo", "崩坏：星穹铁道"), "GraphicsSettings_PCResolution_h431323223");
  ASSERT_EQ(blob.kind, sparxie::RegValue::Kind::binary);
  const json resolution = json::parse(std::string(blob.bytes.begin(), blob.bytes.end()).c_str());
  EXPECT_EQ(resolution["width"], 2868);
  EXPECT_EQ(resolution["height"], 1320);
  EXPECT_EQ(resolution["isFullScreen"], true);
  EXPECT_EQ(reg.get(key_of("miHoYo", "崩坏：星穹铁道"), "Screenmanager Fullscreen mode_h3630240806").dword, 1u) << "Unity's full-screen window";
}

TEST(StreamDisplay, TheSessionSizeIsUsedWhenTheDisplayCannotBeRead) {
  const fs::path root = fresh("sparxie-sd-session");
  sparxie::HostEnv env = env_at(root);
  sparxie::MemoryRegistry reg;
  env.registry = &reg;
  touch(root / "game" / "StarRail.exe");
  json block = block_of(root, "starrail", "cn", "StarRail.exe", "touch", { { "profile", "" } });
  write_apps(env, { { "name", "S" }, { "cmd", "x" }, { "sparxie", block } });
  ASSERT_TRUE(sparxie::prepare_launch(env, "S", "x", { 2400, 1080, 60 }).ok);
  EXPECT_EQ(read_back(install_of(block), reg).resolution, "2400×1080");
  // nothing is known: the stored size stays, the window is the full-screen one
  sparxie::MemoryRegistry other;
  env.registry = &other;
  other.set_dword(key_of("miHoYo", "崩坏：星穹铁道"), "Screenmanager Resolution Width_h182942802", 1280);
  other.set_dword(key_of("miHoYo", "崩坏：星穹铁道"), "Screenmanager Resolution Height_h2627697771", 720);
  other.set_dword(key_of("miHoYo", "崩坏：星穹铁道"), "Screenmanager Fullscreen mode_h3630240806", 3);
  ASSERT_TRUE(sparxie::prepare_launch(env, "S", "x").ok);
  const auto values = read_back(install_of(block), other);
  EXPECT_EQ(values.resolution, "1280×720");
  EXPECT_EQ(values.display, "fullscreen");
}

TEST(StreamDisplay, AFixedDisplayWritesThePersonsSizeAndWindowMode) {
  const fs::path root = fresh("sparxie-sd-fixed");
  sparxie::HostEnv env = env_at(root);
  sparxie::MemoryRegistry reg;
  env.registry = &reg;
  env.display_size = [] { return std::pair<int, int> { 2868, 1320 }; };
  touch(root / "game" / "YuanShen.exe");
  json block = block_of(root, "genshin", "cn", "YuanShen.exe", "pc", { { "display", { { "mode", "fixed" }, { "resolution", "1600×900" }, { "window", "windowed" } } } });
  write_apps(env, { { "name", "G" }, { "cmd", "x" }, { "sparxie", block } });
  ASSERT_TRUE(sparxie::prepare_launch(env, "G", "x", { 2868, 1320, 60 }).ok);
  const auto values = read_back(install_of(block), reg);
  EXPECT_EQ(values.resolution, "1600×900");
  EXPECT_EQ(values.display, "windowed");
}

TEST(StreamDisplay, ZenlessFollowsThroughItsListAndUnrealThroughItsFile) {
  const fs::path root = fresh("sparxie-sd-others");
  sparxie::HostEnv env = env_at(root);
  sparxie::MemoryRegistry reg;
  env.registry = &reg;
  env.display_size = [] { return std::pair<int, int> { 2868, 1320 }; };
  // Wuthering Waves: GameUserSettings.ini
  const fs::path ini = root / "game" / "Client" / "Saved" / "Config" / "WindowsNoEditor" / "GameUserSettings.ini";
  fs::create_directories(ini.parent_path());
  std::ofstream(ini) << "[/Script/Engine.GameUserSettings]\nFullscreenMode=3\nResolutionSizeX=1280\nResolutionSizeY=720\nFrameRateLimit=60\n";
  touch(root / "game" / "Wuthering Waves.exe");
  json block = block_of(root, "wuwa", "cn", "Wuthering Waves.exe", "pc");
  write_apps(env, { { "name", "W" }, { "cmd", "x" }, { "sparxie", block } });
  ASSERT_TRUE(sparxie::prepare_launch(env, "W", "x").ok);
  std::ifstream in(ini);
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_NE(text.find("ResolutionSizeX=2868"), std::string::npos) << text;
  EXPECT_NE(text.find("ResolutionSizeY=1320"), std::string::npos) << text;
  EXPECT_NE(text.find("FullscreenMode=1"), std::string::npos) << text;
  EXPECT_NE(text.find("FrameRateLimit=60"), std::string::npos) << "the other items are the game's own";
  // Endfield: no settings of its own are written, and nothing fails
  touch(root / "game" / "Endfield.exe");
  json endfield = block_of(root, "endfield", "cn", "Endfield.exe", "pc");
  write_apps(env, { { "name", "E" }, { "cmd", "x" }, { "sparxie", endfield } });
  EXPECT_TRUE(sparxie::prepare_launch(env, "E", "x").ok);
}

TEST(StreamDisplay, ADirectStartGetsTheArgumentsAndATouchStartDoesNot) {
  const fs::path root = fresh("sparxie-sd-args");
  sparxie::HostEnv env = env_at(root);
  sparxie::MemoryRegistry reg;
  env.registry = &reg;
  env.display_size = [] { return std::pair<int, int> { 2868, 1320 }; };
  touch(root / "game" / "StarRail.exe");
  const std::string cmd = "\"" + (root / "game" / "StarRail.exe").string() + "\"";
  write_apps(env, { { "name", "S" }, { "cmd", cmd }, { "sparxie", block_of(root, "starrail", "cn", "StarRail.exe", "pc") } });
  std::string args;
  ASSERT_TRUE(sparxie::prepare_launch(env, "S", cmd, {}, &args).ok);
  EXPECT_EQ(args, " -screen-width 2868 -screen-height 1320 -screen-fullscreen 1");
  write_apps(env, { { "name", "S" }, { "cmd", cmd }, { "sparxie", block_of(root, "starrail", "cn", "StarRail.exe", "touch", { { "profile", "" } }) } });
  args = "stale";
  ASSERT_TRUE(sparxie::prepare_launch(env, "S", cmd, {}, &args).ok);
  EXPECT_EQ(args, " --screen-width 2868 --screen-height 1320 --window-mode fullscreen") << "Sparxie's broker starts the game itself: sparxie-launch hands it the size and the window, and the settings above agree";
}

TEST(StreamDisplay, CreateAndEditKeepTheChoiceAndFixedSizesReachTheCommand) {
  const fs::path root = fresh("sparxie-sd-create");
  sparxie::HostEnv env = env_at(root);
  touch(root / "game" / "YuanShen.exe");
  const std::string exe = (root / "game" / "YuanShen.exe").string();
  auto follow = sparxie::create_app(env, { { "path", exe }, { "region", "cn" }, { "mode", "pc" }, { "name", "follow" }, { "settings", { { "resolution", "1280x720" } } } });
  ASSERT_TRUE(follow.status.ok) << follow.status.error;
  EXPECT_EQ(follow.body["app"]["sparxie"]["display"], sparxie::default_display()) << "a new app follows the stream";
  EXPECT_EQ(follow.body["app"]["cmd"].get<std::string>().find("-screen-width"), std::string::npos) << "its size is resolved at launch, not stored";
  auto fixed = sparxie::create_app(env, { { "path", exe }, { "region", "cn" }, { "mode", "pc" }, { "name", "fixed" }, { "display", { { "mode", "fixed" }, { "resolution", "1600x900" }, { "window", "windowed" } } } });
  ASSERT_TRUE(fixed.status.ok) << fixed.status.error;
  EXPECT_EQ(fixed.body["app"]["sparxie"]["display"]["window"], "windowed");
  const std::string cmd = fixed.body["app"]["cmd"];
  EXPECT_NE(cmd.find("-screen-width 1600"), std::string::npos) << cmd;
  EXPECT_NE(cmd.find("-screen-height 900"), std::string::npos) << cmd;
  EXPECT_NE(cmd.find("-screen-fullscreen 0"), std::string::npos) << cmd;
  // the edit: back to following
  const json apps = [&] {
    std::ifstream in(env.apps_file);
    json body;
    in >> body;
    return body;
  }();
  int index = -1;
  for (std::size_t i = 0; i < apps["apps"].size(); ++i)
    if (apps["apps"][i]["name"] == "fixed") index = static_cast<int>(i);
  ASSERT_GE(index, 0);
  auto edited = sparxie::update_app(env, index, { { "display", { { "mode", "follow" } } } });
  ASSERT_TRUE(edited.status.ok) << edited.status.error;
  EXPECT_EQ(edited.body["app"]["sparxie"]["display"], sparxie::default_display());
  EXPECT_EQ(edited.body["app"]["cmd"].get<std::string>().find("-screen-width"), std::string::npos);
}

TEST(StreamDisplay, TheSettingsAnswerNamesTheStreamOnlyWhileOneRuns) {
  const fs::path root = fresh("sparxie-sd-answer");
  sparxie::HostEnv env = env_at(root);
  sparxie::MemoryRegistry reg;
  env.registry = &reg;
  env.display_size = [] { return std::pair<int, int> { 2868, 1320 }; };
  env.display_refresh = [] { return 120; };
  touch(root / "game" / "YuanShen.exe");
  const std::string exe = (root / "game" / "YuanShen.exe").string();
  auto idle = sparxie::game_settings_for(env, "", exe, "cn");
  ASSERT_TRUE(idle.status.ok) << idle.status.error;
  EXPECT_FALSE(idle.body.contains("stream"));
  env.stream_running = [] { return true; };
  auto live = sparxie::game_settings_for(env, "", exe, "cn");
  EXPECT_EQ(live.body["stream"], (json { { "width", 2868 }, { "height", 1320 }, { "refresh", 120 } }));
}

// ---- the size and window as launch arguments (both levers agree), and the session end ----
TEST(ScreenArguments, ThePlanFollowsTheStreamOrTheFixedChoice) {
  const sparxie::StreamMode phone { 2868, 1320, 120 };
  const auto follow = sparxie::plan_screen("starrail", sparxie::DisplayChoice {}, phone);
  EXPECT_EQ(follow.width, 2868);
  EXPECT_EQ(follow.height, 1320);
  EXPECT_EQ(follow.window, "fullscreen");
  EXPECT_EQ(sparxie::unity_screen_arguments(follow), " -screen-width 2868 -screen-height 1320 -screen-fullscreen 1");
  EXPECT_EQ(sparxie::launcher_screen_arguments(follow), " --screen-width 2868 --screen-height 1320 --window-mode fullscreen");
  const auto unknown = sparxie::plan_screen("genshin", sparxie::DisplayChoice {}, {});
  EXPECT_EQ(sparxie::unity_screen_arguments(unknown), " -screen-fullscreen 1") << "an unknown stream size sends the window only";
  sparxie::DisplayChoice fixed;
  fixed.follow = false;
  fixed.resolution = "1280×720";
  fixed.window = "windowed";
  EXPECT_EQ(sparxie::unity_screen_arguments(sparxie::plan_screen("zzz", fixed, phone)), " -screen-width 1280 -screen-height 720 -screen-fullscreen 0") << "Collapse passes -screen-width/-screen-height to Zenless Zone Zero too";
  fixed.window = "borderless";
  fixed.resolution = "1920x1080";
  EXPECT_EQ(sparxie::unity_screen_arguments(sparxie::plan_screen("hi3", fixed, phone)), " -screen-width 1920 -screen-height 1080 -screen-fullscreen 0 -popupwindow");
  EXPECT_EQ(sparxie::launcher_screen_arguments(sparxie::plan_screen("hi3", fixed, phone)), " --screen-width 1920 --screen-height 1080 --window-mode borderless");
  // a Unity title's borderless window is a framed-less window: the registry says windowed, the arguments add -popupwindow
  EXPECT_EQ(sparxie::plan_display("starrail", fixed, phone).window, "windowed");
  EXPECT_EQ(sparxie::plan_display("wuwa", fixed, phone).window, "borderless") << "Unreal has a borderless mode of its own";
  fixed.resolution = "wide";
  EXPECT_EQ(sparxie::unity_screen_arguments(sparxie::plan_screen("genshin", fixed, phone)), " -screen-fullscreen 0 -popupwindow") << "a size that cannot be read is not sent";
  EXPECT_TRUE(sparxie::plan_screen("wuwa", sparxie::DisplayChoice {}, phone).empty()) << "the Unreal titles take no Unity arguments";
  EXPECT_FALSE(sparxie::plan_screen("endfield", sparxie::DisplayChoice {}, phone).empty()) << "Endfield is a Unity game sized through its Sparxie session";
}

TEST(ScreenArguments, OnlyTheGamesOwnCommandTakesThem) {
  const sparxie::StreamMode phone { 2868, 1320, 120 };
  const sparxie::DisplayChoice follow;
  const char *exe = "\"C:\\Games\\StarRail.exe\"";
  EXPECT_FALSE(sparxie::stream_launch_arguments("starrail", exe, follow, phone).empty());
  EXPECT_TRUE(sparxie::stream_launch_arguments("starrail", "steam://rungameid/1", follow, phone).empty()) << "a store address";
  EXPECT_TRUE(sparxie::stream_launch_arguments("starrail", "C:\\Launcher\\Launcher.bat", follow, phone).empty());
  EXPECT_TRUE(sparxie::stream_launch_arguments("starrail", std::string(exe) + " -screen-width 640", follow, phone).empty()) << "a command that already has a size";
  EXPECT_FALSE(sparxie::stream_launch_arguments("zzz", "C:\\Games\\ZenlessZoneZero.exe", follow, phone).empty());
  const std::string launcher = "C:\\S\\sparxie-launch.exe --sparxie C:\\S\\Sparxie.exe --profile p";
  EXPECT_EQ(sparxie::stream_launch_arguments("genshin", launcher, follow, phone, true), " --screen-width 2868 --screen-height 1320 --window-mode fullscreen");
  EXPECT_TRUE(sparxie::stream_launch_arguments("genshin", launcher + " --screen-width 1", follow, phone, true).empty());
  EXPECT_TRUE(sparxie::stream_launch_arguments("wuwa", launcher, follow, phone, true).empty());
  const std::string direct = "\"C:\\S\\sparxie-launch.exe\" --endfield-direct \"C:\\E\\Endfield.exe\"";
  EXPECT_EQ(sparxie::stream_launch_arguments("endfield", direct, follow, phone), " -- -screen-width 2868 -screen-height 1320 -screen-fullscreen 1") << "the decoy-parent direct start gets the plan after the launcher's separator";
  sparxie::DisplayChoice borderless;
  borderless.follow = false;
  borderless.resolution = "1920\xC3\x97" "1080";
  borderless.window = "borderless";
  EXPECT_EQ(sparxie::stream_launch_arguments("endfield", direct, borderless, phone), " -- -screen-width 1920 -screen-height 1080 -screen-fullscreen 0 -popupwindow");
  EXPECT_TRUE(sparxie::stream_launch_arguments("endfield", direct, follow, {}).empty()) << "following a stream of unknown size sends nothing";
  EXPECT_TRUE(sparxie::stream_launch_arguments("endfield", direct + " -- -screen-width 640 -screen-height 480", follow, phone).empty()) << "a command that already has a game part is not extended";
}

TEST(ScreenArguments, ALaunchSendsTheFixedWindowToTheGameAndTheRegistryAgrees) {
  const fs::path root = fresh("sparxie-sa-fixed");
  sparxie::HostEnv env = env_at(root);
  sparxie::MemoryRegistry reg;
  env.registry = &reg;
  env.display_size = [] { return std::pair<int, int> { 2868, 1320 }; };
  touch(root / "game" / "StarRail.exe");
  const std::string cmd = "\"" + (root / "game" / "StarRail.exe").string() + "\"";
  json block = block_of(root, "starrail", "cn", "StarRail.exe", "pc", { { "display", { { "mode", "fixed" }, { "resolution", "1280×720" }, { "window", "windowed" } } } });
  write_apps(env, { { "name", "S" }, { "cmd", cmd }, { "sparxie", block } });
  std::string args;
  ASSERT_TRUE(sparxie::prepare_launch(env, "S", cmd, { 2868, 1320, 60 }, &args).ok);
  EXPECT_EQ(args, " -screen-width 1280 -screen-height 720 -screen-fullscreen 0");
  const auto values = read_back(install_of(block), reg);
  EXPECT_EQ(values.resolution, "1280×720") << "the registry says the same as the command line";
  EXPECT_EQ(values.display, "windowed");
  EXPECT_EQ(reg.get(key_of("miHoYo", "崩坏：星穹铁道"), "Screenmanager Fullscreen mode_h3630240806").dword, 3u) << "Star Rail's windowed mode, as Collapse writes it";
  // the same app through Sparxie: the launcher gets the numbers
  block["mode"] = "touch";
  block["profile"] = "";
  write_apps(env, { { "name", "S" }, { "cmd", cmd }, { "sparxie", block } });
  ASSERT_TRUE(sparxie::prepare_launch(env, "S", cmd, {}, &args).ok);
  EXPECT_EQ(args, " --screen-width 1280 --screen-height 720 --window-mode windowed");
}

namespace {
  // an application of Star Rail that follows the stream (the game's window and size are what a launch writes)
  struct EndFixture {
    fs::path root;
    sparxie::HostEnv env;
    sparxie::MemoryRegistry reg;
    json block;
    std::string key = key_of("miHoYo", "崩坏：星穹铁道");
    bool running = false;
    explicit EndFixture(const char *name, const json &extra = json::object()) {
      root = fresh(name);
      env = env_at(root);
      env.registry = &reg;
      env.display_size = [] { return std::pair<int, int> { 2868, 1320 }; };
      env.process_running = [this](const std::vector<std::string> &) { return running; };
      touch(root / "game" / "StarRail.exe");
      block = block_of(root, "starrail", "cn", "StarRail.exe", "pc", extra);
      write_apps(env, { { "name", "S" }, { "cmd", "x" }, { "sparxie", block } });
      reg.set_dword(key, "Screenmanager Resolution Width_h182942802", 1280);
      reg.set_dword(key, "Screenmanager Resolution Height_h2627697771", 720);
      reg.set_dword(key, "Screenmanager Fullscreen mode_h3630240806", 3);
      reg.set_dword(key, "Screenmanager Resolution Use Native_h1405027254", 1);
    }
    std::uint32_t dword(const char *name) { return reg.get(key, name).dword; }
  };
}  // namespace

TEST(SessionEnd, TheValuesTheLaunchWroteGoBackWhenTheSessionEnds) {
  EndFixture f("sparxie-se-basic");
  ASSERT_TRUE(sparxie::prepare_launch(f.env, "S", "x", { 2868, 1320, 60 }).ok);
  const auto started = sparxie::launch_generation();
  EXPECT_EQ(f.dword("Screenmanager Resolution Width_h182942802"), 2868u);
  EXPECT_EQ(f.dword("Screenmanager Fullscreen mode_h3630240806"), 1u);
  EXPECT_EQ(f.dword("Screenmanager Resolution Use Native_h1405027254"), 0u);
  ASSERT_TRUE(sparxie::end_session(f.env, "S", "x", started).ok);
  EXPECT_EQ(f.dword("Screenmanager Resolution Width_h182942802"), 1280u);
  EXPECT_EQ(f.dword("Screenmanager Resolution Height_h2627697771"), 720u);
  EXPECT_EQ(f.dword("Screenmanager Fullscreen mode_h3630240806"), 3u);
  EXPECT_EQ(f.dword("Screenmanager Resolution Use Native_h1405027254"), 1u);
  EXPECT_FALSE(fs::exists(f.env.host_data / "game-settings-backup" / "starrail-cn.json")) << "the backup is gone once it was put back";
  EXPECT_TRUE(sparxie::end_session(f.env, "S", "x", started).ok) << "ending twice is harmless";
  EXPECT_EQ(f.dword("Screenmanager Resolution Width_h182942802"), 1280u);
}

TEST(SessionEnd, AValueTheGameChangedDuringPlayStaysAndTheRestGoesBack) {
  EndFixture f("sparxie-se-game-changed");
  ASSERT_TRUE(sparxie::prepare_launch(f.env, "S", "x", { 2868, 1320, 60 }).ok);
  const auto started = sparxie::launch_generation();
  // the person changed the size in the game's own menu while playing: the game stored it
  f.reg.set_dword(f.key, "Screenmanager Resolution Width_h182942802", 1600);
  f.reg.set_dword(f.key, "Screenmanager Resolution Height_h2627697771", 900);
  ASSERT_TRUE(sparxie::end_session(f.env, "S", "x", started).ok);
  EXPECT_EQ(f.dword("Screenmanager Resolution Width_h182942802"), 1600u) << "the game's own change is kept";
  EXPECT_EQ(f.dword("Screenmanager Resolution Height_h2627697771"), 900u);
  EXPECT_EQ(f.dword("Screenmanager Fullscreen mode_h3630240806"), 3u) << "what the game did not touch goes back";
  EXPECT_EQ(f.dword("Screenmanager Resolution Use Native_h1405027254"), 1u);
}

TEST(SessionEnd, NothingIsPutBackWhileTheGameRunsAndItIsDoneWhenItQuits) {
  EndFixture f("sparxie-se-running");
  ASSERT_TRUE(sparxie::prepare_launch(f.env, "S", "x", { 2868, 1320, 60 }).ok);
  const auto started = sparxie::launch_generation();
  f.running = true;
  const auto busy = sparxie::end_session(f.env, "S", "x", started);
  EXPECT_FALSE(busy.ok);
  EXPECT_EQ(busy.error_code, "game_running");
  EXPECT_EQ(f.dword("Screenmanager Resolution Width_h182942802"), 2868u) << "the game owns its settings while it runs";
  EXPECT_FALSE(sparxie::finish_session(f.env, "S", "x", started, std::chrono::milliseconds(0), std::chrono::milliseconds(1)));
  // the game quits a moment later: the waiting restore goes through
  std::thread quit([&f] {
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    f.running = false;
  });
  EXPECT_TRUE(sparxie::finish_session(f.env, "S", "x", started, std::chrono::seconds(10), std::chrono::milliseconds(20)));
  quit.join();
  EXPECT_EQ(f.dword("Screenmanager Resolution Width_h182942802"), 1280u);
}

TEST(SessionEnd, ALateEndNeverUndoesANewerLaunch) {
  EndFixture f("sparxie-se-generation");
  ASSERT_TRUE(sparxie::prepare_launch(f.env, "S", "x", { 2868, 1320, 60 }).ok);
  const auto first = sparxie::launch_generation();
  ASSERT_TRUE(sparxie::prepare_launch(f.env, "S", "x", { 2868, 1320, 60 }).ok);  // the same game starts again before the first end was handled
  EXPECT_NE(sparxie::launch_generation(), first);
  ASSERT_TRUE(sparxie::end_session(f.env, "S", "x", first).ok);
  EXPECT_EQ(f.dword("Screenmanager Resolution Width_h182942802"), 2868u) << "the first session's end leaves the second launch's values alone";
  ASSERT_TRUE(sparxie::end_session(f.env, "S", "x", sparxie::launch_generation()).ok);
  EXPECT_EQ(f.dword("Screenmanager Resolution Width_h182942802"), 1280u) << "the second session's end puts back the original values, not the first launch's";
}

TEST(SessionEnd, KeepLeavesTheValuesAndDropsTheBackup) {
  EndFixture f("sparxie-se-keep", { { "restore", "keep" } });
  ASSERT_TRUE(sparxie::prepare_launch(f.env, "S", "x", { 2868, 1320, 60 }).ok);
  ASSERT_TRUE(sparxie::end_session(f.env, "S", "x", sparxie::launch_generation()).ok);
  EXPECT_EQ(f.dword("Screenmanager Resolution Width_h182942802"), 2868u);
  EXPECT_FALSE(fs::exists(f.env.host_data / "game-settings-backup" / "starrail-cn.json"));
}

TEST(SessionEnd, AHostThatWentDownMidSessionPutsBackAtItsNextStart) {
  EndFixture f("sparxie-se-recover");
  ASSERT_TRUE(sparxie::prepare_launch(f.env, "S", "x", { 2868, 1320, 60 }).ok);
  f.running = true;  // the game is still there when the host starts again: its values stay
  EXPECT_EQ(sparxie::recover_pending_settings(f.env), 0);
  EXPECT_EQ(f.dword("Screenmanager Resolution Width_h182942802"), 2868u);
  f.running = false;
  EXPECT_EQ(sparxie::recover_pending_settings(f.env), 1);
  EXPECT_EQ(f.dword("Screenmanager Resolution Width_h182942802"), 1280u);
}

TEST(SessionEnd, ItemsOfAJsonValueTheGameRewroteAreCompared_NotTheWholeValue) {
  // Genshin keeps most settings in one JSON value (with JSON inside it) that the game rewrites when it quits
  const std::string key = key_of("miHoYo", "原神");
  const fs::path root = fresh("sparxie-se-blob");
  sparxie::HostEnv env = env_at(root);
  sparxie::MemoryRegistry reg;
  env.registry = &reg;
  env.display_size = [] { return std::pair<int, int> { 2868, 1320 }; };
  touch(root / "game" / "YuanShen.exe");
  json block = block_of(root, "genshin", "cn", "YuanShen.exe", "pc", { { "settings", { { "fps", 30 }, { "hdr", true } } } });
  write_apps(env, { { "name", "G" }, { "cmd", "x" }, { "sparxie", block } });
  const json perf = { { "saveItems", json::array({ { { "entryType", 1 }, { "index", 2 } }, { { "entryType", 7 }, { "index", 1 } } }) } };
  const json general = { { "volumeGlobal", 10 }, { "enableHDR", false }, { "globalPerfData", perf.dump() } };
  auto put = [&](const json &body) {
    const std::string text = body.dump();
    std::vector<std::uint8_t> bytes(text.begin(), text.end());
    bytes.push_back(0);
    reg.set_binary(key, "GENERAL_DATA_h2389025596", bytes);
  };
  auto get = [&] {
    const auto value = reg.get(key, "GENERAL_DATA_h2389025596");
    return json::parse(std::string(value.bytes.begin(), value.bytes.end()).c_str());
  };
  put(general);
  reg.set_dword(key, "WINDOWS_HDR_ON_h3132281285", 0);
  ASSERT_TRUE(sparxie::prepare_launch(env, "G", "x", { 2868, 1320, 60 }).ok);
  json written = get();
  EXPECT_EQ(written["enableHDR"], true);
  EXPECT_EQ(reg.get(key, "WINDOWS_HDR_ON_h3132281285").dword, 1u);
  // the game quits and rewrites the whole value: our items stay as written, and it changed things of its own
  written["volumeGlobal"] = 5;
  json written_perf = json::parse(written["globalPerfData"].get<std::string>());
  written_perf["saveItems"].push_back({ { "entryType", 99 }, { "index", 1 } });
  written["globalPerfData"] = written_perf.dump();
  put(written);
  ASSERT_TRUE(sparxie::end_session(env, "G", "x", sparxie::launch_generation()).ok);
  const json after = get();
  EXPECT_EQ(after["enableHDR"], false) << "the HDR item the launch wrote goes back";
  EXPECT_EQ(after["volumeGlobal"], 5) << "an item the game changed stays";
  const json after_perf = json::parse(after["globalPerfData"].get<std::string>());
  bool fps_back = false, game_item = false;
  for (const auto &item : after_perf["saveItems"]) {
    if (item["entryType"] == 1) fps_back = item["index"] == 2;
    if (item["entryType"] == 99) game_item = true;
  }
  EXPECT_TRUE(fps_back) << after_perf.dump();
  EXPECT_TRUE(game_item) << "an entry the game added stays";
  EXPECT_EQ(reg.get(key, "WINDOWS_HDR_ON_h3132281285").dword, 0u);
}

TEST(SparseSettings, AnApplicationHoldsOnlyWhatWasChangedAndLaunchWritesOnlyThat) {
  const fs::path root = fresh("sparxie-sparse");
  sparxie::HostEnv env = env_at(root);
  sparxie::MemoryRegistry reg;
  env.registry = &reg;
  touch(root / "game" / "YuanShen.exe");
  const std::string exe = (root / "game" / "YuanShen.exe").string();
  auto created = sparxie::create_app(env, { { "path", exe }, { "region", "cn" }, { "mode", "pc" }, { "name", "G" }, { "settings", { { "vsync", true }, { "bogus", 1 } } } });
  ASSERT_TRUE(created.status.ok) << created.status.error;
  const json &block = created.body["app"]["sparxie"];
  EXPECT_EQ(block["settings"], (json { { "vsync", true } })) << "only the known item that was sent";
  EXPECT_FALSE(block.contains("settingsMode"));
  auto none = sparxie::create_app(env, { { "path", exe }, { "region", "cn" }, { "mode", "pc" }, { "name", "G2" } });
  ASSERT_TRUE(none.status.ok);
  EXPECT_TRUE(none.body["app"]["sparxie"]["settings"].empty()) << "a new application follows the game";
  // back to following the game, per item and for all
  const auto cleared = sparxie::update_app(env, 0, { { "settings", json::object() } });
  ASSERT_TRUE(cleared.status.ok) << cleared.status.error;
  EXPECT_TRUE(cleared.body["app"]["sparxie"]["settings"].empty());
  // an update that sends no settings changes none
  ASSERT_TRUE(sparxie::update_app(env, 0, { { "settings", { { "fps", 30 } } } }).status.ok);
  const auto renamed = sparxie::update_app(env, 0, { { "name", "G renamed" } });
  ASSERT_TRUE(renamed.status.ok);
  EXPECT_EQ(renamed.body["app"]["sparxie"]["settings"], (json { { "fps", 30 } }));
}

// ---- Arknights: Endfield: the game applies its own saved display after it starts, so the launch writes it (1112d) ----
namespace {
  struct EndfieldFixture {
    fs::path root;
    sparxie::HostEnv env;
    sparxie::MemoryRegistry reg;
    json block;
    std::string key = key_of("Hypergryph", "Endfield");
    explicit EndfieldFixture(const char *name, const json &extra = json::object(), const char *mode = "touch") {
      root = fresh(name);
      env = env_at(root);
      env.registry = &reg;
      env.display_size = [] { return std::pair<int, int> { 2868, 1320 }; };
      env.process_running = [](const std::vector<std::string> &) { return false; };
      touch(root / "game" / "Endfield.exe");
      block = block_of(root, "endfield", "cn", "Endfield.exe", mode, extra);
      write_apps(env, { { "name", "E" }, { "cmd", "x" }, { "sparxie", block } });
      // what a captured registry held: the game's own display settings, a window of 1280x720
      reg.set_dword(key, "Screenmanager Resolution Width_h182942802", 1280);
      reg.set_dword(key, "Screenmanager Resolution Height_h2627697771", 720);
      reg.set_dword(key, "Screenmanager Fullscreen mode_h3630240806", 3);
      reg.set_dword(key, "Screenmanager Resolution Use Native_h1405027254", 0);
      reg.set_dword(key, "Screenmanager Resolution Window Width_h2524650974", 1280);
      reg.set_dword(key, "Screenmanager Resolution Window Height_h1684712807", 720);
      reg.set_dword(key, "video_resolution_width_h583690364", 1280);
      reg.set_dword(key, "video_resolution_height_h2517654917", 720);
      reg.set_dword(key, "video_full_screen_h1998742411", 0);
      reg.set_dword(key, "video_resolution_h1805082501", 1);
    }
    std::uint32_t dword(const char *name) { return reg.get(key, name).dword; }
  };
}  // namespace

TEST(EndfieldDisplay, FollowTheStreamWritesTheGamesOwnKeysAndTheLauncherGetsTheSameNumbers) {
  EndfieldFixture f("sparxie-ef-follow");
  std::string args;
  ASSERT_TRUE(sparxie::prepare_launch(f.env, "E", "x", { 2868, 1320, 120 }, &args).ok);
  EXPECT_EQ(args, " --screen-width 2868 --screen-height 1320 --window-mode fullscreen");
  EXPECT_EQ(f.dword("video_resolution_width_h583690364"), 2868u) << "the game's own size item: this is what turned the start into 1280x720";
  EXPECT_EQ(f.dword("video_resolution_height_h2517654917"), 1320u);
  EXPECT_EQ(f.dword("video_full_screen_h1998742411"), 1u) << "the game's own full-screen item";
  EXPECT_EQ(f.dword("Screenmanager Resolution Width_h182942802"), 2868u);
  EXPECT_EQ(f.dword("Screenmanager Resolution Height_h2627697771"), 1320u);
  EXPECT_EQ(f.dword("Screenmanager Fullscreen mode_h3630240806"), 1u) << "Unity's full-screen window";
  EXPECT_EQ(f.dword("Screenmanager Resolution Use Native_h1405027254"), 0u);
  EXPECT_EQ(f.dword("video_resolution_h1805082501"), 1u) << "the game's resolution list position is not ours to move";
  // the session ends: the person's own window comes back
  ASSERT_TRUE(sparxie::end_session(f.env, "E", "x", sparxie::launch_generation()).ok);
  EXPECT_EQ(f.dword("video_resolution_width_h583690364"), 1280u);
  EXPECT_EQ(f.dword("video_resolution_height_h2517654917"), 720u);
  EXPECT_EQ(f.dword("video_full_screen_h1998742411"), 0u);
  EXPECT_EQ(f.dword("Screenmanager Fullscreen mode_h3630240806"), 3u);
  EXPECT_EQ(f.dword("Screenmanager Resolution Width_h182942802"), 1280u);
}

TEST(EndfieldDisplay, AFixedWindowKeepsItsSizeInTheWindowedMemoryToo) {
  EndfieldFixture f("sparxie-ef-fixed", { { "display", { { "mode", "fixed" }, { "resolution", "1600×900" }, { "window", "windowed" } } } });
  std::string args;
  ASSERT_TRUE(sparxie::prepare_launch(f.env, "E", "x", { 2868, 1320, 120 }, &args).ok);
  EXPECT_EQ(args, " --screen-width 1600 --screen-height 900 --window-mode windowed");
  EXPECT_EQ(f.dword("video_resolution_width_h583690364"), 1600u);
  EXPECT_EQ(f.dword("video_full_screen_h1998742411"), 0u);
  EXPECT_EQ(f.dword("Screenmanager Fullscreen mode_h3630240806"), 3u);
  EXPECT_EQ(f.dword("Screenmanager Resolution Window Width_h2524650974"), 1600u);
  EXPECT_EQ(f.dword("Screenmanager Resolution Window Height_h1684712807"), 900u);
}

TEST(EndfieldDisplay, ABorderlessWindowIsAWindowWithoutAFrame) {
  EndfieldFixture f("sparxie-ef-borderless", { { "display", { { "mode", "fixed" }, { "resolution", "1920×1080" }, { "window", "borderless" } } } });
  std::string args;
  ASSERT_TRUE(sparxie::prepare_launch(f.env, "E", "x", {}, &args).ok);
  EXPECT_EQ(f.dword("video_full_screen_h1998742411"), 0u);
  EXPECT_EQ(f.dword("Screenmanager Fullscreen mode_h3630240806"), 3u);
  EXPECT_EQ(args, " --screen-width 1920 --screen-height 1080 --window-mode borderless");
}

TEST(EndfieldDisplay, AnUnknownStreamSizeWritesTheWindowModeOnly) {
  EndfieldFixture f("sparxie-ef-unknown");
  f.env.display_size = [] { return std::pair<int, int> { 0, 0 }; };
  ASSERT_TRUE(sparxie::prepare_launch(f.env, "E", "x").ok);
  EXPECT_EQ(f.dword("video_full_screen_h1998742411"), 1u);
  EXPECT_EQ(f.dword("video_resolution_width_h583690364"), 1280u) << "no size is known: the stored one stays";
}

TEST(EndfieldDisplay, AGameThatIsRunningIsLeftAlone) {
  EndfieldFixture f("sparxie-ef-running");
  f.env.process_running = [](const std::vector<std::string> &) { return true; };
  const auto status = sparxie::prepare_launch(f.env, "E", "x", { 2868, 1320, 120 });
  EXPECT_FALSE(status.ok);
  EXPECT_EQ(f.dword("video_resolution_width_h583690364"), 1280u);
}

// ---- Endfield has no automatic input; an app that still says so is migrated once; sparxie-launch apps end with the launcher ----
TEST(EndfieldInput, AnAppThatStillSaysAutoBecomesTouchOrKeyboardOnce) {
  const fs::path root = fresh("sparxie-ef-auto-migrate");
  sparxie::HostEnv env = env_at(root);
  touch(root / "game" / "Endfield.exe");
  json touch_app = { { "name", "Endfield touch" }, { "cmd", "\"C:\\S\\sparxie-launch.exe\" --sparxie \"C:\\S\\Sparxie.exe\" --profile endfield-cn-1" }, { "sparxie", block_of(root, "endfield", "cn", "Endfield.exe", "touch", { { "options", { { "input", "auto" }, { "fps", 120 } } }, { "profile", "" } }) } };
  json pc_app = { { "name", "Endfield kbm" }, { "cmd", "\"C:\\S\\sparxie-launch.exe\" --endfield-direct \"C:\\G\\Endfield.exe\"" }, { "sparxie", block_of(root, "endfield", "cn", "Endfield.exe", "pc", { { "options", { { "input", "auto" } } } }) } };
  json other = { { "name", "Genshin" }, { "cmd", "C:\\G\\YuanShen.exe" }, { "sparxie", block_of(root, "genshin", "cn", "YuanShen.exe", "pc", { { "options", { { "input", "kbm" } } } }) } };
  std::ofstream(env.apps_file) << json { { "apps", { touch_app, pc_app, other } } }.dump();
  sparxie::attach_official_covers(env);
  std::ifstream in(env.apps_file);
  json apps;
  in >> apps;
  EXPECT_EQ(apps["apps"][0]["sparxie"]["options"]["input"], "touch") << "a touch app was automatic by the client's device: touch";
  EXPECT_EQ(apps["apps"][1]["sparxie"]["options"]["input"], "kbm") << "a direct app: keyboard, mouse and gamepad";
  EXPECT_EQ(apps["apps"][2]["sparxie"]["options"]["input"], "kbm") << "other titles are untouched";
  EXPECT_EQ(apps["apps"][0]["sparxie"]["options"]["fps"], 120);
  sparxie::attach_official_covers(env);  // once: the second start changes nothing
  std::ifstream again(env.apps_file);
  json second;
  again >> second;
  EXPECT_EQ(second["apps"][0]["sparxie"]["options"]["input"], "touch");
}

TEST(SparxieLaunchApps, EndWhenTheLauncherDoes) {
  const fs::path root = fresh("sparxie-launch-wait-all");
  sparxie::HostEnv env = env_at(root);
  touch(root / "game" / "StarRail.exe");
  const std::string launcher_cmd = "\"C:\\S\\sparxie-launch.exe\" --sparxie \"C:\\S\\Sparxie.exe\" --profile starrail-cn-1";
  json launcher_app = { { "name", "Star Rail touch" }, { "cmd", launcher_cmd }, { "sparxie", block_of(root, "starrail", "cn", "StarRail.exe", "touch", { { "profile", "starrail-cn-1" } }) } };
  json direct_app = { { "name", "Star Rail direct" }, { "cmd", "C:\\G\\StarRail.exe" }, { "sparxie", block_of(root, "starrail", "cn", "StarRail.exe", "pc") } };
  json explicit_app = { { "name", "Kept" }, { "cmd", launcher_cmd }, { "wait-all", true }, { "sparxie", block_of(root, "starrail", "cn", "StarRail.exe", "touch", { { "profile", "starrail-cn-1" } }) } };
  std::ofstream(env.apps_file) << json { { "apps", { launcher_app, direct_app, explicit_app } } }.dump();
  sparxie::attach_official_covers(env);
  std::ifstream in(env.apps_file);
  json apps;
  in >> apps;
  ASSERT_TRUE(apps["apps"][0].contains("wait-all"));
  EXPECT_EQ(apps["apps"][0]["wait-all"], false);
  EXPECT_FALSE(apps["apps"][1].contains("wait-all")) << "a game started directly is the app's own process";
  EXPECT_EQ(apps["apps"][2]["wait-all"], true) << "what the person set stays";
  // and a new launcher app is made that way
  sparxie::Install install;
  install.game = "starrail";
  install.dir = (root / "game").string();
  const json made = sparxie::make_app("n", launcher_cmd, true, install, json::object());
  EXPECT_EQ(made["wait-all"], false);
  EXPECT_FALSE(sparxie::make_app("n", "C:\\G\\StarRail.exe", true, install, json::object()).contains("wait-all"));
}

TEST(EndfieldDisplay, TheDirectStartGetsTheWindowAfterTheSeparatorAndEveryChoiceIsOffered) {
  const sparxie::StreamMode phone { 2868, 1320, 120 };
  const std::string direct = "\"C:\\\\S\\\\sparxie-launch.exe\" --endfield-direct \"C:\\\\G\\\\Endfield.exe\"";
  sparxie::DisplayChoice follow;
  EXPECT_EQ(sparxie::stream_launch_arguments("endfield", direct, follow, phone), " -- -screen-width 2868 -screen-height 1320 -screen-fullscreen 1");
  sparxie::DisplayChoice fixed;
  fixed.follow = false;
  fixed.resolution = "1600×900";
  fixed.window = "windowed";
  EXPECT_EQ(sparxie::stream_launch_arguments("endfield", direct, fixed, phone), " -- -screen-width 1600 -screen-height 900 -screen-fullscreen 0");
  fixed.window = "borderless";
  EXPECT_EQ(sparxie::stream_launch_arguments("endfield", direct, fixed, phone), " -- -screen-width 1600 -screen-height 900 -screen-fullscreen 0 -popupwindow");
  const std::string touch = "\"C:\\\\S\\\\sparxie-launch.exe\" --sparxie \"C:\\\\S\\\\Sparxie.exe\" --profile endfield-cn-1";
  EXPECT_EQ(sparxie::stream_launch_arguments("endfield", touch, fixed, phone, true), " --screen-width 1600 --screen-height 900 --window-mode borderless");
  // the stored choice keeps what the page sends: the three window modes, a size
  for (const char *window : { "fullscreen", "borderless", "windowed" }) {
    const json stored = sparxie::normalized_display({ { "mode", "fixed" }, { "resolution", "1920x1080" }, { "window", window } });
    EXPECT_EQ(stored["window"], window);
    EXPECT_EQ(stored["resolution"], "1920×1080");
  }
}
