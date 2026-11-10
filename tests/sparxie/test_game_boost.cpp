#include "src/sparxie/host.h"
#include "src/sparxie/hoyo_settings.h"
#include "src/sparxie/platform.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#endif

namespace fs = std::filesystem;

// 提升游戏优先级: the host raises the game process to above normal once it runs, on both launch paths, and Sparxie's own profile priority follows it
namespace {
  struct Fixture {
    fs::path root;
    sparxie::HostEnv env;
    std::string exe;

    explicit Fixture(const char *name) {
      root = fs::temp_directory_path() / name;
      fs::remove_all(root);
      fs::create_directories(root / "game" / "YuanShen_Data" / "Plugins");
      std::ofstream(root / "game" / "YuanShen.exe") << "x";
      std::ofstream(root / "game" / "YuanShen_Data" / "Plugins" / "PCGameSDK.dll") << "x";
      env.host_data = root / "host";
      env.data_dir = root / "sparxie-data";
      env.apps_file = root / "apps.json";
      env.covers_dir = root / "covers";
      env.install_root = root / "install";
      fs::create_directories(root / "install" / "components" / "sparxie");
      std::ofstream(root / "install" / "components" / "sparxie" / "Sparxie.exe") << "x";
      std::ofstream(root / "install" / "components" / "sparxie" / "component.json") << R"({"version":"0.9.0"})";
      env.process_running = [](const std::vector<std::string> &) { return false; };
      exe = (root / "game" / "YuanShen.exe").string();
    }

    sparxie::ApiResult
    make(const char *mode, const char *settings_mode, bool boost, const char *name) {
      sparxie::json request = { { "path", exe }, { "mode", mode }, { "name", name }, { "settings", std::string(settings_mode) == "preset" ? sparxie::json { { "vsync", true }, { "gameBoost", boost } } : sparxie::json::object() } };
      return sparxie::create_app(env, request);
    }

    std::string
    sparxie_priority() const {
      std::ifstream in(root / "sparxie-data" / "config.json");
      sparxie::json document;
      in >> document;
      return document["profiles"][0]["hoyo"].value("processPriority", std::string());
    }
  };
}  // namespace

TEST(GameBoost, TheBoostCountsOnlyWhenTheApplicationHoldsItAsAChangedItem) {
  EXPECT_TRUE(sparxie::game_boost_on({ { "gameBoost", true } }));
  EXPECT_FALSE(sparxie::game_boost_on({ { "gameBoost", false } }));
  EXPECT_FALSE(sparxie::game_boost_on(sparxie::json::object())) << "an application that changed nothing follows the game";
  EXPECT_FALSE(sparxie::game_boost_on({ { "vsync", true } }));
  EXPECT_FALSE(sparxie::game_boost_on("gameBoost"));
}

TEST(GameBoost, SparxiePriorityIsRaisedToAboveNormalAndNeverLowered) {
  EXPECT_EQ(sparxie::options_with_boost({ { "priority", "normal" } }, true)["priority"], "aboveNormal");
  EXPECT_EQ(sparxie::options_with_boost({ { "priority", "belowNormal" } }, true)["priority"], "aboveNormal");
  EXPECT_EQ(sparxie::options_with_boost(sparxie::json::object(), true)["priority"], "aboveNormal") << "no priority stored reads as normal";
  EXPECT_EQ(sparxie::options_with_boost({ { "priority", "high" } }, true)["priority"], "high");
  EXPECT_EQ(sparxie::options_with_boost({ { "priority", "normal" } }, false)["priority"], "normal");
  EXPECT_FALSE(sparxie::options_with_boost(sparxie::json::object(), false).contains("priority"));
}

TEST(GameBoost, ADirectStartWatchesTheGameExecutable) {
  Fixture f("sparxie-boost-direct");
  fs::path watched;
  std::chrono::milliseconds waited { 0 };
  f.env.raise_priority = [&](const fs::path &exe, std::chrono::milliseconds wait) {
    watched = exe;
    waited = wait;
    return 1;
  };
  const auto created = f.make("pc", "preset", true, "Genshin direct");
  ASSERT_TRUE(created.status.ok) << created.status.error;
  const std::string name = created.body["app"]["name"];
  const std::string cmd = created.body["app"]["cmd"];
  EXPECT_EQ(sparxie::run_launch_boost(f.env, name, cmd, std::chrono::seconds(7)), 1);
  EXPECT_EQ(watched, fs::path(f.exe));
  EXPECT_EQ(waited, std::chrono::seconds(7));
}

TEST(GameBoost, NothingIsRaisedWithoutTheSwitchOrInAutomaticMode) {
  Fixture f("sparxie-boost-off");
  int calls = 0;
  f.env.raise_priority = [&](const fs::path &, std::chrono::milliseconds) {
    ++calls;
    return 1;
  };
  const auto off = f.make("pc", "preset", false, "Genshin off");
  ASSERT_TRUE(off.status.ok) << off.status.error;
  EXPECT_EQ(sparxie::run_launch_boost(f.env, off.body["app"]["name"], off.body["app"]["cmd"], std::chrono::seconds(1)), 0);
  const auto automatic = f.make("pc", "auto", true, "Genshin auto");
  ASSERT_TRUE(automatic.status.ok) << automatic.status.error;
  EXPECT_EQ(sparxie::run_launch_boost(f.env, automatic.body["app"]["name"], automatic.body["app"]["cmd"], std::chrono::seconds(1)), 0);
  EXPECT_EQ(sparxie::run_launch_boost(f.env, "no such app", "nothing", std::chrono::seconds(1)), 0);
  EXPECT_EQ(calls, 0);
}

TEST(GameBoost, ASparxieStartWatchesTheSameExecutableAndTheProfilePriorityFollowsTheSwitch) {
  Fixture f("sparxie-boost-touch");
  std::vector<std::string> watched;
  f.env.raise_priority = [&](const fs::path &exe, std::chrono::milliseconds) {
    watched.push_back(exe.string());
    return 1;
  };
  const auto created = f.make("touch", "preset", true, "Genshin touch");
  ASSERT_TRUE(created.status.ok) << created.status.error;
  EXPECT_EQ(f.sparxie_priority(), "aboveNormal") << "Sparxie's runtime would put the game back to normal otherwise";
  EXPECT_EQ(sparxie::run_launch_boost(f.env, created.body["app"]["name"], created.body["app"]["cmd"], std::chrono::seconds(1)), 1);
  ASSERT_EQ(watched.size(), 1u);
  EXPECT_EQ(watched[0], f.exe);
  // the switch goes off in a later save that changes the settings only: Sparxie's priority goes back with it
  const auto off = sparxie::update_app(f.env, 0, { { "settings", { { "vsync", true }, { "gameBoost", false } } } });
  ASSERT_TRUE(off.status.ok) << off.status.error;
  EXPECT_EQ(f.sparxie_priority(), "normal");
  const auto on = sparxie::update_app(f.env, 0, { { "settings", { { "vsync", true }, { "gameBoost", true } } } });
  ASSERT_TRUE(on.status.ok) << on.status.error;
  EXPECT_EQ(f.sparxie_priority(), "aboveNormal");
  // a priority the person chose in Sparxie stays when it is already higher
  sparxie::json options = { { "priority", "high" }, { "input", "touch" } };
  const auto high = sparxie::update_app(f.env, 0, { { "options", options } });
  ASSERT_TRUE(high.status.ok) << high.status.error;
  EXPECT_EQ(f.sparxie_priority(), "high");
}

TEST(GameBoost, CustomArgumentsMayBeSeveralLines) {
  EXPECT_EQ(sparxie::custom_args_problem("-screen-quality 2\n-force-d3d11\r\n"), "");
  EXPECT_FALSE(sparxie::custom_args_problem("calc.exe & whoami").empty());
  EXPECT_FALSE(sparxie::custom_args_problem("-a\n|b").empty());
  sparxie::Install install;
  install.game = "genshin";
  install.exe_path = "D:/G/YuanShen.exe";
  const std::string command = sparxie::game_launch_command("\"D:/G/YuanShen.exe\"", install, { { "customArgs", "-screen-quality 2\n-force-d3d11" } });
  EXPECT_NE(command.find("\"-screen-quality\" \"2\" \"-force-d3d11\""), std::string::npos) << command;
}

TEST(GameBoost, TheGameSettingsAnswerCarriesTheStreamedDisplayWhenTheHostCanReadIt) {
  Fixture f("sparxie-boost-screen");
  const auto none = sparxie::game_settings_for(f.env, "", f.exe, "bili");
  ASSERT_TRUE(none.status.ok) << none.status.error;
  EXPECT_FALSE(none.body.contains("screen")) << "no display size, no screen";
  f.env.display_size = [] { return std::pair<int, int> { 2560, 1440 }; };
  const auto known = sparxie::game_settings_for(f.env, "", f.exe, "bili");
  ASSERT_TRUE(known.status.ok) << known.status.error;
  EXPECT_EQ(known.body["screen"]["width"], 2560);
  EXPECT_EQ(known.body["screen"]["height"], 1440);
  f.env.display_size = [] { return std::pair<int, int> { 0, 0 }; };
  EXPECT_FALSE(sparxie::game_settings_for(f.env, "", f.exe, "bili").body.contains("screen")) << "an unreadable display is not sent as zeros";
}

#if defined(_WIN32)
// the real thing: a process whose image is the game's executable is found and raised, and one with another image is left alone
TEST(GameBoostWindows, ARunningProcessOfThatImageGoesToAboveNormal) {
  const fs::path dir = fs::temp_directory_path() / "sparxie-boost-real";
  fs::remove_all(dir);
  fs::create_directories(dir);
  wchar_t system_dir[MAX_PATH] = {};
  GetSystemDirectoryW(system_dir, MAX_PATH);
  const fs::path game = dir / "FakeGame.exe";
  const fs::path other = dir / "OtherGame.exe";
  fs::copy_file(fs::path(system_dir) / "ping.exe", game);
  fs::copy_file(fs::path(system_dir) / "ping.exe", other);
  auto start = [](const fs::path &exe) {
    std::wstring command = L"\"" + exe.wstring() + L"\" -n 25 127.0.0.1";
    STARTUPINFOW startup {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION info {};
    EXPECT_TRUE(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info)) << GetLastError();
    return info;
  };
  PROCESS_INFORMATION a = start(game);
  PROCESS_INFORMATION b = start(other);
  EXPECT_EQ(GetPriorityClass(a.hProcess), static_cast<DWORD>(NORMAL_PRIORITY_CLASS));
  const int raised = sparxie::host_raise_game_priority(game, std::chrono::seconds(6));
  EXPECT_EQ(raised, 1);
  EXPECT_EQ(GetPriorityClass(a.hProcess), static_cast<DWORD>(ABOVE_NORMAL_PRIORITY_CLASS));
  EXPECT_EQ(GetPriorityClass(b.hProcess), static_cast<DWORD>(NORMAL_PRIORITY_CLASS)) << "another image stays as it was";
  // an image that never starts: nothing raised, the call returns after the wait
  const auto before = std::chrono::steady_clock::now();
  EXPECT_EQ(sparxie::host_raise_game_priority(dir / "NeverStarts.exe", std::chrono::milliseconds(1200)), 0);
  EXPECT_GE(std::chrono::steady_clock::now() - before, std::chrono::milliseconds(1000));
  TerminateProcess(a.hProcess, 0);
  TerminateProcess(b.hProcess, 0);
  CloseHandle(a.hProcess);
  CloseHandle(a.hThread);
  CloseHandle(b.hProcess);
  CloseHandle(b.hThread);
}
#endif
