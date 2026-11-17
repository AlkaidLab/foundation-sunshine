// The session-end restore against the real Windows registry (a subtree of its own: HKCU\Software\SparxieE2E1112c\..., never a game's key). Windows only; other hosts compile nothing.
#if defined(_WIN32)
  #include "src/sparxie/host.h"
  #include "src/sparxie/platform.h"
  #include <filesystem>
  #include <fstream>
  #include <gtest/gtest.h>
  #include <windows.h>
namespace fs = std::filesystem;
using sparxie::json;

namespace {
  const char *kPrefix = "Software\\SparxieE2E1112c\\";
  void
  wipe() {
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\SparxieE2E1112c");
  }
  struct Cleanup {
    Cleanup() { wipe(); }
    ~Cleanup() { wipe(); }
  };
  std::string
  star_rail_key() {
    return std::string("Software\\miHoYo\\") + "崩坏：星穹铁道";
  }
}  // namespace

TEST(WindowsRegistry, ASessionLaunchesWritesAndPutsBackThroughTheRealRegistry) {
  Cleanup cleanup;
  sparxie::PrefixedRegistry reg(sparxie::host_registry(), kPrefix);
  const fs::path root = fs::temp_directory_path() / "sparxie-winreg-e2e";
  fs::remove_all(root);
  fs::create_directories(root / "game");
  std::ofstream(root / "game" / "StarRail.exe") << "x";
  sparxie::HostEnv env;
  env.host_data = root / "host";
  env.data_dir = root / "data";
  env.apps_file = root / "apps.json";
  env.registry = &reg;
  env.display_size = [] { return std::pair<int, int> { 2868, 1320 }; };
  env.process_running = [](const std::vector<std::string> &) { return false; };
  const std::string key = star_rail_key();
  json block = { { "install", "starrail-cn" }, { "game", "starrail" }, { "region", "cn" }, { "dir", (root / "game").string() }, { "exe", (root / "game" / "StarRail.exe").string() }, { "mode", "pc" }, { "restore", "restore" } };
  std::ofstream(env.apps_file) << json { { "apps", { { { "name", "S" }, { "cmd", "x" }, { "sparxie", block } } } } }.dump();
  reg.set_dword(key, "Screenmanager Resolution Width_h182942802", 1280);
  reg.set_dword(key, "Screenmanager Resolution Height_h2627697771", 720);
  reg.set_dword(key, "Screenmanager Fullscreen mode_h3630240806", 3);
  reg.set_dword(key, "Screenmanager Resolution Use Native_h1405027254", 1);
  std::string args;
  ASSERT_TRUE(sparxie::prepare_launch(env, "S", "x", { 2868, 1320, 60 }, &args).ok);
  EXPECT_EQ(reg.get(key, "Screenmanager Resolution Width_h182942802").dword, 2868u);
  EXPECT_EQ(reg.get(key, "Screenmanager Fullscreen mode_h3630240806").dword, 1u);
  EXPECT_EQ(reg.get(key, "GraphicsSettings_PCResolution_h431323223").kind, sparxie::RegValue::Kind::binary) << "the launch created the game's size object";
  ASSERT_TRUE(sparxie::end_session(env, "S", "x", sparxie::launch_generation()).ok);
  EXPECT_EQ(reg.get(key, "Screenmanager Resolution Width_h182942802").dword, 1280u);
  EXPECT_EQ(reg.get(key, "Screenmanager Resolution Height_h2627697771").dword, 720u);
  EXPECT_EQ(reg.get(key, "Screenmanager Fullscreen mode_h3630240806").dword, 3u);
  EXPECT_EQ(reg.get(key, "Screenmanager Resolution Use Native_h1405027254").dword, 1u);
  EXPECT_EQ(reg.get(key, "GraphicsSettings_PCResolution_h431323223").kind, sparxie::RegValue::Kind::none) << "a value the launch created is removed again";
  fs::remove_all(root);
}
#endif
