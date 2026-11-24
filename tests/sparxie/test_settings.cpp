#include "src/sparxie/settings.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
namespace fs = std::filesystem;
TEST(Sleepy, RoundTrip) {
  const std::vector<std::uint8_t> magic { 0x01, 0xC0, 0x22, 0x05 };
  const std::string text = R"({"SystemSettingDataMap":{"110":{"Data":1}},"keep":true})";
  const auto encoded = sparxie::sleepy_write(text, magic);
  EXPECT_EQ(sparxie::sleepy_read(encoded, magic), text);
  EXPECT_TRUE(sparxie::sleepy_read(encoded, {}).empty());
}
TEST(Settings, GenshinWriteAndRefuseWhileRunning) {
  sparxie::MemoryRegistry reg;
  sparxie::Install install;
  install.game = "genshin";
  install.region = "cn";
  install.id = "genshin-cn";
  const std::string root = std::string("Software") + static_cast<char>(92) + "miHoYo" + static_cast<char>(92) + "原神";
  const std::string perf = R"({"saveItems":[{"entryType":1,"index":1,"itemVersion":"CNRELWin5.0.0"}],"portedVersion":"CNRELWin5.0.0"})";
  sparxie::json general = { { "enableHDR", false }, { "globalPerfData", perf }, { "account", "keep-me" } };
  const std::string dumped = general.dump();
  reg.set_binary(root, "GENERAL_DATA_h2389025596", std::vector<std::uint8_t>(dumped.begin(), dumped.end()));
  sparxie::GameValues values;
  values.resolution = "1280x720";
  values.display = "windowed";
  values.fps = 30;
  values.vsync = true;
  values.render_scale = 0.8;
  values.preset = "low";
  values.hdr = true;
  const fs::path backup = fs::temp_directory_path() / "sparxie-settings-backup";
  fs::remove_all(backup);
  auto running = sparxie::write_game_settings(install, reg, values, {}, [] { return true; }, backup);
  EXPECT_EQ(running.status.error_code, "game_running");
  EXPECT_EQ(reg.get(root, "Screenmanager Resolution Width_h182942802").kind, sparxie::RegValue::Kind::none);
  auto wrote = sparxie::write_game_settings(install, reg, values, {}, [] { return false; }, backup);
  EXPECT_TRUE(wrote.status.ok) << wrote.status.error;
  EXPECT_EQ(reg.get(root, "Screenmanager Resolution Width_h182942802").dword, 1280u);
  EXPECT_EQ(reg.get(root, "Screenmanager Is Fullscreen mode_h3981298716").dword, 0u);
  EXPECT_EQ(reg.get(root, "WINDOWS_HDR_ON_h3132281285").dword, 1u);
  const auto blob = reg.get(root, "GENERAL_DATA_h2389025596");
  auto stored = sparxie::json::parse(std::string(blob.bytes.begin(), blob.bytes.end()));
  EXPECT_EQ(stored["account"], "keep-me");
  auto read = sparxie::read_game_settings(install, reg, {}, [] { return false; });
  EXPECT_EQ(read.values.fps, 30);
  EXPECT_TRUE(read.values.vsync);
  EXPECT_EQ(read.values.display, "windowed");
  EXPECT_TRUE(read.values.hdr);
  values.display = "borderless";
  auto bad = sparxie::write_game_settings(install, reg, values, {}, [] { return false; }, {});
  EXPECT_EQ(bad.status.error_code, "invalid_input");
}
TEST(Settings, StarRailWindowAndMissingModel) {
  sparxie::MemoryRegistry reg;
  sparxie::Install install;
  install.game = "starrail";
  install.region = "global";
  install.id = "starrail-global";
  sparxie::GameValues values;
  values.resolution = "2560x1440";
  values.display = "windowed";
  values.fps = 120;
  values.preset = "high";
  auto wrote = sparxie::write_game_settings(install, reg, values, {}, [] { return false; }, {});
  EXPECT_TRUE(wrote.status.ok) << wrote.status.error;
  const std::string root = std::string("Software") + static_cast<char>(92) + "Cognosphere" + static_cast<char>(92) + "Star Rail";
  EXPECT_EQ(reg.get(root, "Screenmanager Fullscreen mode_h3630240806").dword, 3u);
  EXPECT_EQ(reg.get(root, "GraphicsSettings_GraphicsQuality_h523255858").dword, 4u);
  EXPECT_NE(std::find(wrote.unavailable.begin(), wrote.unavailable.end(), "fps"), wrote.unavailable.end());
  EXPECT_EQ(std::find(wrote.unavailable.begin(), wrote.unavailable.end(), "hdr"), wrote.unavailable.end());
}

// Star Rail on PC offers 30 and 60 in its own menu; 120 is an unlock that Collapse writes as FPS=120 into the graphics model. The host writes exactly that and leaves the other keys alone.
TEST(Settings, StarRail120IsWrittenAsFpsOnlyAndTheRestOfTheModelStays) {
  sparxie::MemoryRegistry reg;
  sparxie::Install starrail;
  starrail.game = "starrail";
  starrail.region = "cn";
  starrail.id = "starrail-cn";
  const std::string root = std::string("Software") + static_cast<char>(92) + "miHoYo" + static_cast<char>(92) + "崩坏：星穹铁道";
  const sparxie::json before = { { "FPS", 60 }, { "EnableVSync", true }, { "RenderScale", 1.2 }, { "ShadowQuality", 3 }, { "EnableSelfShadow", 1 }, { "Nested", { { "a", { 1, 2 } } } } };
  const std::string text = before.dump();
  std::vector<std::uint8_t> bytes(text.begin(), text.end());
  bytes.push_back(0);
  reg.set_binary(root, "GraphicsSettings_Model_h2986158309", bytes);
  reg.set_dword(root, "GraphicsSettings_GraphicsQuality_h523255858", 4);
  sparxie::GameValues values;
  values.fps = 120;
  const auto wrote = sparxie::write_game_settings(starrail, reg, values, {}, [] { return false; }, {}, "keep", { "fps" });
  ASSERT_TRUE(wrote.status.ok) << wrote.status.error;
  EXPECT_NE(std::find(wrote.applied.begin(), wrote.applied.end(), "fps"), wrote.applied.end());
  const auto blob = reg.get(root, "GraphicsSettings_Model_h2986158309");
  std::string stored(blob.bytes.begin(), blob.bytes.end());
  while (!stored.empty() && stored.back() == '\0') stored.pop_back();
  sparxie::json after = sparxie::json::parse(stored);
  EXPECT_EQ(after["FPS"], 120);
  after["FPS"] = 60;
  EXPECT_EQ(after, before) << "every other key of the model is as it was";
  EXPECT_EQ(reg.get(root, "GraphicsSettings_GraphicsQuality_h523255858").dword, 4u) << "the quality preset is not touched by a frame-rate write";
  const auto read = sparxie::read_game_settings(starrail, reg, {}, [] { return false; });
  EXPECT_EQ(read.values.fps, 120);
  values.fps = 90;
  EXPECT_EQ(sparxie::write_game_settings(starrail, reg, values, {}, [] { return false; }, {}, "keep", { "fps" }).status.error_code, "invalid_input") << "only 30, 60 and 120";
}

TEST(Settings, GenshinWritesThe45TierAndStarRailHasNoHdr) {
  sparxie::MemoryRegistry reg;
  sparxie::Install genshin;
  genshin.game = "genshin";
  genshin.region = "cn";
  genshin.id = "genshin-cn";
  const std::string root = std::string("Software") + static_cast<char>(92) + "miHoYo" + static_cast<char>(92) + "原神";
  const std::string perf = R"({"saveItems":[{"entryType":1,"index":1,"itemVersion":"CNRELWin5.0.0"}],"portedVersion":"CNRELWin5.0.0"})";
  sparxie::json general = { { "enableHDR", false }, { "globalPerfData", perf } };
  const std::string dumped = general.dump();
  reg.set_binary(root, "GENERAL_DATA_h2389025596", std::vector<std::uint8_t>(dumped.begin(), dumped.end()));
  sparxie::GameValues values;
  values.fps = 45;
  auto wrote = sparxie::write_game_settings(genshin, reg, values, {}, [] { return false; }, {});
  EXPECT_TRUE(wrote.status.ok) << wrote.status.error;
  auto read = sparxie::read_game_settings(genshin, reg, {}, [] { return false; });
  EXPECT_EQ(read.values.fps, 45);
  const auto blob = reg.get(root, "GENERAL_DATA_h2389025596");
  auto stored = sparxie::json::parse(std::string(blob.bytes.begin(), blob.bytes.end()));
  const auto saved_perf = sparxie::json::parse(stored["globalPerfData"].get<std::string>());
  bool tier = false;
  for (const auto &item : saved_perf["saveItems"])
    if (item.value("entryType", -1) == 1) tier = item.value("index", -1) == 2;
  EXPECT_TRUE(tier);

  sparxie::Install starrail;
  starrail.game = "starrail";
  starrail.region = "cn";
  starrail.id = "starrail-cn";
  const std::string rail_root = std::string("Software") + static_cast<char>(92) + "miHoYo" + static_cast<char>(92) + "崩坏：星穹铁道";
  const std::string model = R"({"FPS":60,"EnableVSync":false,"RenderScale":1.0})";
  reg.set_binary(rail_root, "GraphicsSettings_Model_h2986158309", std::vector<std::uint8_t>(model.begin(), model.end()));
  reg.set_dword(rail_root, "GraphicsSettings_GraphicsQuality_h523255858", 4);
  auto rail = sparxie::read_game_settings(starrail, reg, {}, [] { return false; });
  EXPECT_TRUE(rail.status.ok);
  EXPECT_TRUE(rail.unavailable.empty());
}

TEST(Settings, RestoreUnchangedAndKeepEdits) {
  sparxie::MemoryRegistry reg;
  sparxie::Install install;
  install.game = "genshin";
  install.region = "cn";
  install.id = "genshin-cn";
  const std::string root = std::string("Software") + static_cast<char>(92) + "miHoYo" + static_cast<char>(92) + "原神";
  reg.set_dword(root, "Screenmanager Resolution Width_h182942802", 1920);
  reg.set_dword(root, "Screenmanager Resolution Height_h2627697771", 1080);
  reg.set_dword(root, "Screenmanager Is Fullscreen mode_h3981298716", 1);
  const std::string perf = R"({"saveItems":[{"entryType":1,"index":1,"itemVersion":"CNRELWin5.0.0"}],"portedVersion":"CNRELWin5.0.0"})";
  sparxie::json general = { { "enableHDR", false }, { "globalPerfData", perf }, { "account", "keep-me" } };
  const std::string dumped = general.dump();
  reg.set_binary(root, "GENERAL_DATA_h2389025596", std::vector<std::uint8_t>(dumped.begin(), dumped.end()));
  const fs::path backup = fs::temp_directory_path() / "sparxie-settings-restore";
  fs::remove_all(backup);
  sparxie::GameValues values;
  values.resolution = "1280x720";
  values.display = "windowed";
  values.fps = 30;
  values.preset = "low";
  values.render_scale = 0.8;
  auto wrote = sparxie::write_game_settings(install, reg, values, {}, [] { return false; }, backup, "restore");
  EXPECT_TRUE(wrote.status.ok) << wrote.status.error;
  EXPECT_EQ(reg.get(root, "Screenmanager Resolution Width_h182942802").dword, 1280u);
  sparxie::reconcile_pending_settings(install, reg, backup, {});
  EXPECT_EQ(reg.get(root, "Screenmanager Resolution Width_h182942802").dword, 1920u);
  EXPECT_EQ(reg.get(root, "Screenmanager Is Fullscreen mode_h3981298716").dword, 1u);
  EXPECT_FALSE(fs::exists(backup / "genshin-cn.json"));
  wrote = sparxie::write_game_settings(install, reg, values, {}, [] { return false; }, backup, "restore");
  EXPECT_TRUE(wrote.status.ok);
  reg.set_dword(root, "Screenmanager Resolution Width_h182942802", 1600);
  sparxie::reconcile_pending_settings(install, reg, backup, {});
  EXPECT_EQ(reg.get(root, "Screenmanager Resolution Width_h182942802").dword, 1600u);
  EXPECT_FALSE(fs::exists(backup / "genshin-cn.json"));
  wrote = sparxie::write_game_settings(install, reg, values, {}, [] { return false; }, backup, "keep");
  EXPECT_TRUE(wrote.status.ok);
  EXPECT_EQ(reg.get(root, "Screenmanager Resolution Width_h182942802").dword, 1280u);
  sparxie::reconcile_pending_settings(install, reg, backup, {});
  EXPECT_EQ(reg.get(root, "Screenmanager Resolution Width_h182942802").dword, 1280u);
  EXPECT_FALSE(fs::exists(backup / "genshin-cn.json"));
}

TEST(Settings, UnrealFixtureUsesAtomicIniRoundTripAndRestoresUnchangedFile) {
  const fs::path root = fs::temp_directory_path() / "sparxie-unreal-settings";
  fs::remove_all(root);
  const fs::path file = root / "Client" / "Saved" / "Config" / "WindowsNoEditor" / "GameUserSettings.ini";
  fs::create_directories(file.parent_path());
  {
    std::ofstream out(file);
    out << "[/Script/Engine.GameUserSettings]\n"
        << "ResolutionSizeX=1920\nResolutionSizeY=1080\nFullscreenMode=1\n"
        << "FrameRateLimit=60.000000\nbUseVSync=False\nsg.ResolutionQuality=100.000000\nOverallQuality=3\n";
  }
  sparxie::Install install;
  install.game = "wuwa";
  install.region = "cn";
  install.id = "wuwa-cn";
  install.dir = root.string();
  sparxie::MemoryRegistry registry;
  const auto before = sparxie::read_game_settings(install, registry, {}, [] { return false; });
  EXPECT_TRUE(before.status.ok);
  EXPECT_EQ(before.values.resolution, "1920×1080");
  EXPECT_EQ(before.values.fps, 60);
  sparxie::GameValues values = before.values;
  values.resolution = "2560x1440";
  values.display = "borderless";
  values.fps = 120;
  values.vsync = true;
  values.render_scale = 125;
  values.preset = "high";
  const fs::path backup = root / "backup";
  const auto wrote = sparxie::write_game_settings(install, registry, values, {}, [] { return false; }, backup, "restore", { "resolution", "display", "fps", "vsync", "renderScale", "preset" });
  ASSERT_TRUE(wrote.status.ok) << wrote.status.error;
  const auto after = sparxie::read_game_settings(install, registry, {}, [] { return false; });
  EXPECT_EQ(after.values.resolution, "2560×1440");
  EXPECT_EQ(after.values.display, "borderless");
  EXPECT_EQ(after.values.fps, 120);
  EXPECT_TRUE(after.values.vsync);
  sparxie::reconcile_pending_settings(install, registry, backup, {});
  const auto restored = sparxie::read_game_settings(install, registry, {}, [] { return false; });
  EXPECT_EQ(restored.values.resolution, "1920×1080");
  EXPECT_EQ(restored.values.fps, 60);
}
