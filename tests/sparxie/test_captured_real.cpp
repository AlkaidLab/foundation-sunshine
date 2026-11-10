#include "src/sparxie/host.h"
#include "src/sparxie/hoyo_settings.h"
#include "src/sparxie/settings.h"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

namespace fs = std::filesystem;
using sparxie::json;

// Real values captured from a test host. tests/sparxie/fixtures/sample-registry-20261112.json is the game keys of its registry (the game-settings values only),
// sample-zzz-general-data-20261112.json the decoded GENERAL_DATA.bin of Zenless Zone Zero (account number removed), sample-apps-20261112.json the Sparxie applications of its apps.json (sanitised).
namespace {
  std::string
  slurp(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  }
  fs::path
  fixture(const char *name) { return fs::path(SPARXIE_FIXTURES_DIR) / name; }
  fs::path
  fresh(const char *name) {
    const fs::path root = fs::temp_directory_path() / name;
    fs::remove_all(root);
    fs::create_directories(root);
    return root;
  }
  std::string
  root_of(const char *vendor, const char *name) { return std::string("Software") + static_cast<char>(92) + vendor + static_cast<char>(92) + name; }
  const std::vector<std::uint8_t> kMagic = [] {
    const char *hex = "556ed19674d183ce956e67696ed0b52e47d0b06d65ce9f626a65d18174";
    const auto digit = [](char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; };
    std::vector<std::uint8_t> out;
    for (const char *p = hex; p[0] && p[1]; p += 2) out.push_back(static_cast<std::uint8_t>(digit(p[0]) << 4 | digit(p[1])));
    return out;
  }();

  struct Title {
    const char *game;
    const char *fixture_key;  // the key name in the registry fixture
    const char *vendor;
    const char *name;
  };
  const Title kGenshin { "genshin", "原神", "miHoYo", "原神" };
  const Title kStarRail { "starrail", "崩坏：星穹铁道", "miHoYo", "崩坏：星穹铁道" };
  const Title kZzz { "zzz", "绝区零", "miHoYo", "绝区零" };

  void
  load_registry(sparxie::MemoryRegistry &reg, const Title &title) {
    const json all = json::parse(slurp(fixture("sample-registry-20261112.json")));
    const std::string root = root_of(title.vendor, title.name);
    for (auto it = all[title.fixture_key].begin(); it != all[title.fixture_key].end(); ++it) {
      const std::string kind = it.value()["kind"], value = it.value()["value"];
      if (kind == "DWord")
        reg.set_dword(root, it.key(), static_cast<std::uint32_t>(std::stoll(value)));
      else if (kind == "String")
        reg.set_text(root, it.key(), value);
      else {
        std::vector<std::uint8_t> bytes(value.begin(), value.end());
        bytes.push_back(0);
        reg.set_binary(root, it.key(), bytes);
      }
    }
  }
  sparxie::Install
  install_of(const Title &title, const fs::path &dir) {
    sparxie::Install install;
    install.id = std::string(title.game) + "-cn";
    install.game = title.game;
    install.region = "cn";
    install.dir = dir.string();
    return install;
  }
  // the ZZZ file under a game folder, encoded again from the decoded document
  void
  place_zzz_file(const fs::path &dir) {
    const fs::path file = dir / "ZenlessZoneZero_Data" / "Persistent" / "LocalStorage" / "GENERAL_DATA.bin";
    fs::create_directories(file.parent_path());
    const std::vector<std::uint8_t> bytes = sparxie::sleepy_write(slurp(fixture("sample-zzz-general-data-20261112.json")), kMagic);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
  sparxie::GameValues
  read_values(const sparxie::Install &install, sparxie::Registry &reg) {
    return sparxie::read_game_settings(install, reg, { kMagic }, [] { return false; }).values;
  }
  json
  read_json(const sparxie::Install &install, sparxie::Registry &reg) {
    const auto result = sparxie::read_game_settings(install, reg, { kMagic }, [] { return false; });
    EXPECT_TRUE(result.unavailable.empty()) << "nothing the captured install holds is unreadable";
    return sparxie::values_json(result.values);
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
  // the captured applications, with the paths of the machine that runs the test
  void
  write_sample_apps(const sparxie::HostEnv &env) {
    json apps = json::parse(slurp(fixture("sample-apps-20261112.json")));
    const fs::path game = env.apps_file.parent_path() / "game";
    fs::create_directories(game);
    for (auto &app : apps["apps"]) {
      app["sparxie"]["profile"] = "";  // no Sparxie store here: the host settings are the subject
      app["cmd"] = app["name"];
      const std::string exe = app["sparxie"]["exe"];
      const std::string file = exe.substr(exe.find_last_of(static_cast<char>(92)) + 1);
      std::ofstream(game / file) << "x";
      app["sparxie"]["dir"] = game.string();
      app["sparxie"]["exe"] = (game / file).string();
    }
    std::ofstream(env.apps_file) << apps.dump(2);
  }
  json
  app_named(const sparxie::HostEnv &env, const std::string &name) {
    const json apps = json::parse(slurp(env.apps_file));
    for (const auto &app : apps["apps"])
      if (app["name"] == name) return app;
    return {};
  }
  json
  pc_resolution(sparxie::Registry &reg) {
    const auto blob = reg.get(root_of("miHoYo", "崩坏：星穹铁道"), "GraphicsSettings_PCResolution_h431323223");
    return json::parse(std::string(blob.bytes.begin(), blob.bytes.end()).c_str());
  }
  json
  model(sparxie::Registry &reg) {
    const auto blob = reg.get(root_of("miHoYo", "崩坏：星穹铁道"), "GraphicsSettings_Model_h2986158309");
    return json::parse(std::string(blob.bytes.begin(), blob.bytes.end()).c_str());
  }
  std::uint32_t
  dword(sparxie::Registry &reg, const char *name) { return reg.get(root_of("miHoYo", "崩坏：星穹铁道"), name).dword; }
}  // namespace

TEST(CapturedReal, StarRailValuesAreWhatTheRegistryHolds) {
  sparxie::MemoryRegistry reg;
  load_registry(reg, kStarRail);
  const json v = read_json(install_of(kStarRail, "/x"), reg);
  // GraphicsSettings_Model_h2986158309 of the sample host: DlssQuality 4 (Quality), AAMode 1 (TAA), the six quality items 4 (high), FPS 120
  EXPECT_EQ(v["dlss"], "quality") << "the editor said 关 while the game has DLSS Quality";
  EXPECT_EQ(v["aa"], "taa");
  for (const char *key : { "shadow", "light", "character", "environment", "reflection", "effects", "bloom", "resolutionQuality" }) EXPECT_EQ(v[key], "high") << key;
  EXPECT_EQ(v["fps"], 120);
  EXPECT_EQ(v["renderScale"], 1.0);
  EXPECT_EQ(v["selfShadow"], true);
  EXPECT_EQ(v["halfResTransparent"], false);
  EXPECT_EQ(v["preset"], "high") << "GraphicsSettings_GraphicsQuality 4";
  EXPECT_EQ(v["audioLanguage"], "cn");
  EXPECT_EQ(v["textLanguage"], "cn");
  for (const char *key : { "masterVolume", "musicVolume", "sfxVolume", "voiceVolume" }) EXPECT_EQ(v[key], 10) << key;
  EXPECT_EQ(v["gameBoost"], true) << "CollapseLauncher_Misc UseGameBoost";
  EXPECT_EQ(v["graphicsApi"], "11.1ns");
  EXPECT_FALSE(v.contains("hdr") && v["hdr"] == true);
  // size and window mode: PCResolution {2868x1320, isFullScreen true}
  EXPECT_EQ(v["resolution"], "2868×1320");
  EXPECT_EQ(v["display"], "fullscreen");
}

TEST(CapturedReal, StarRailTakesTheSizeAndWindowFromItsOwnObjectNotFromUnitysCopy) {
  sparxie::MemoryRegistry reg;
  load_registry(reg, kStarRail);
  const std::string root = root_of("miHoYo", "崩坏：星穹铁道");
  // the state the captured registry had before the 08:40 launches: Unity's keys say a 1920×1080 window, the game's own object says full screen
  const std::string text = R"({"width": 1920, "height": 1080, "isFullScreen": true})";
  reg.set_binary(root, "GraphicsSettings_PCResolution_h431323223", std::vector<std::uint8_t>(text.begin(), text.end()));
  reg.set_dword(root, "Screenmanager Fullscreen mode_h3630240806", 3);
  reg.set_dword(root, "Screenmanager Resolution Width_h182942802", 1920);
  reg.set_dword(root, "Screenmanager Resolution Height_h2627697771", 1080);
  const auto values = read_values(install_of(kStarRail, "/x"), reg);
  EXPECT_EQ(values.display, "fullscreen") << "the game starts full screen from its own object, whatever Unity's copy says";
  EXPECT_EQ(values.resolution, "1920×1080");
  // without the object Unity's copy answers
  sparxie::MemoryRegistry bare;
  load_registry(bare, kStarRail);
  bare.set_binary(root, "GraphicsSettings_PCResolution_h431323223", {});
  bare.set_dword(root, "Screenmanager Fullscreen mode_h3630240806", 3);
  EXPECT_EQ(read_values(install_of(kStarRail, "/x"), bare).display, "windowed");
}

TEST(CapturedReal, GenshinValuesAreWhatTheRegistryHolds) {
  sparxie::MemoryRegistry reg;
  load_registry(reg, kGenshin);
  const json v = read_json(install_of(kGenshin, "/x"), reg);
  EXPECT_EQ(v["display"], "windowed") << "Screenmanager Is Fullscreen mode 0";
  EXPECT_EQ(v["resolution"], "1920×1080");
  EXPECT_EQ(v["fps"], 60) << "saveItems entryType 1 index 1";
  EXPECT_EQ(v["vsync"], false) << "entryType 7 index 0";
  EXPECT_EQ(v["renderScale"], 1.0) << "entryType 2 index 2";
  EXPECT_EQ(v["preset"], "high") << "entryType 18 index 3";
  EXPECT_EQ(v["hdr"], false);
  EXPECT_EQ(v["shadow"], "high");
  EXPECT_EQ(v["effects"], "high");
  EXPECT_EQ(v["sfxQuality"], "high");
  EXPECT_EQ(v["environment"], "high");
  EXPECT_EQ(v["aa"], "smaa") << "entryType 8 index 2: Off, FSR 2, SMAA";
  EXPECT_EQ(v["fog"], "on");
  EXPECT_EQ(v["reflections"], "on");
  EXPECT_EQ(v["motionBlur"], "off") << "entryType 11 index 0";
  EXPECT_EQ(v["bloom"], "on");
  EXPECT_EQ(v["crowd"], "high");
  EXPECT_EQ(v["subsurface"], "high");
  EXPECT_EQ(v["coop"], "on");
  EXPECT_EQ(v["anisotropic"], "8");
  EXPECT_EQ(v["gi"], "high");
  EXPECT_EQ(v["dynamicCharacter"], "on");
  for (const char *key : { "masterVolume", "musicVolume", "sfxVolume", "voiceVolume" }) EXPECT_EQ(v[key], 10) << key;
  EXPECT_EQ(v["audioLanguage"], "zh-cn") << "deviceVoiceLanguageType 0";
  EXPECT_EQ(v["textLanguage"], "zh-cn") << "deviceLanguageType 2";
  EXPECT_EQ(v["gameBoost"], false);
}

TEST(CapturedReal, ZenlessValuesAreWhatTheRegistryAndItsFileHold) {
  const fs::path dir = fresh("sparxie-captured-real-zzz");
  place_zzz_file(dir);
  sparxie::MemoryRegistry reg;
  load_registry(reg, kZzz);
  const json v = read_json(install_of(kZzz, dir), reg);
  EXPECT_EQ(v["resolution"], "2868×1320") << "Screenmanager Resolution Width/Height";
  EXPECT_EQ(v["display"], "fullscreen") << "Screenmanager Fullscreen mode 1";
  // GENERAL_DATA.bin: slot 110 = 1 (60 fps), slot 8 = 1 (V-sync on), slot 9 = 1 (1.0), slot 3 = 0 (high)
  EXPECT_EQ(v["fps"], 60);
  EXPECT_EQ(v["vsync"], true);
  EXPECT_EQ(v["renderScale"], 1.0);
  EXPECT_EQ(v["preset"], "high");
  EXPECT_EQ(v["hdr"], false);
  EXPECT_EQ(v["graphicsApi"], "12") << "CollapseLauncher_ScreenSetting GameGraphicsAPI 4";
}

TEST(CapturedReal, TheTouchAppWithAFixedWindowIsAppliedAtLaunchAndPutBackAfterwards) {
  const fs::path root = fresh("sparxie-captured-real-touch");
  sparxie::HostEnv env = env_at(root);
  sparxie::MemoryRegistry reg;
  env.registry = &reg;
  env.display_size = [] { return std::pair<int, int> { 2868, 1320 }; };
  load_registry(reg, kStarRail);
  write_sample_apps(env);
  const std::string sr = root_of("miHoYo", "崩坏：星穹铁道");
  // a session that ended at the screen's own size leaves Unity's flag on: the next start would be at that size whatever is stored
  ASSERT_EQ(dword(reg, "Screenmanager Resolution Use Native_h1405027254"), 1u);
  const json dlss_before = model(reg)["DlssQuality"];
  const json model_before = model(reg);
  const sparxie::Install install = install_of(kStarRail, "/x");
  const sparxie::GameValues original = read_values(install, reg);
  const json before = sparxie::values_json(original);

  // 星铁触屏: display fixed, 1280×720, windowed (the captured block)
  const auto started = sparxie::prepare_launch(env, "星铁触屏", "星铁触屏", { 2868, 1320, 120 });
  ASSERT_TRUE(started.ok) << started.error;
  EXPECT_EQ(pc_resolution(reg), (json { { "height", 720 }, { "isFullScreen", false }, { "width", 1280 } })) << "the game's own object";
  EXPECT_EQ(dword(reg, "Screenmanager Fullscreen mode_h3630240806"), 3u) << "Unity's windowed mode";
  EXPECT_EQ(dword(reg, "Screenmanager Resolution Width_h182942802"), 1280u);
  EXPECT_EQ(dword(reg, "Screenmanager Resolution Height_h2627697771"), 720u);
  EXPECT_EQ(dword(reg, "Screenmanager Resolution Use Native_h1405027254"), 0u) << "a size that is written is the size to start at";
  const auto applied = read_values(install, reg);
  EXPECT_EQ(applied.display, "windowed");
  EXPECT_EQ(applied.resolution, "1280×720");
  EXPECT_EQ(model(reg)["DlssQuality"], dlss_before) << "an item the application does not hold is the game's own";
  // what the preset of this application holds is written with it
  EXPECT_EQ(model(reg)["FPS"], 120);
  EXPECT_EQ(model(reg)["EnableVSync"], false);

  // the next launch of the same game (the other application, follow the stream) puts the old values back first and then writes its own
  const auto second = sparxie::prepare_launch(env, "星穹铁道", "星穹铁道", { 2868, 1320, 120 });
  ASSERT_TRUE(second.ok) << second.error;
  const auto follow = read_values(install, reg);
  EXPECT_EQ(follow.resolution, "2868×1320");
  EXPECT_EQ(follow.display, "fullscreen");
  EXPECT_EQ(dword(reg, "Screenmanager Fullscreen mode_h3630240806"), 1u);

  // and the restore proves the round trip: the captured values of before, key by key
  sparxie::reconcile_pending_settings(install, reg, env.host_data / "game-settings-backup", { kMagic });
  EXPECT_EQ(sparxie::values_json(read_values(install, reg)), before);
  EXPECT_EQ(model(reg), model_before);
  EXPECT_EQ(dword(reg, "Screenmanager Resolution Use Native_h1405027254"), 1u) << "Use Native is part of the restore";
  (void) sr;
}

TEST(CapturedReal, ASaveTakesEffectAtTheNextLaunchWithNoRestart) {
  const fs::path root = fresh("sparxie-captured-real-save");
  sparxie::HostEnv env = env_at(root);
  sparxie::MemoryRegistry reg;
  env.registry = &reg;
  env.display_size = [] { return std::pair<int, int> { 2868, 1320 }; };
  load_registry(reg, kStarRail);
  write_sample_apps(env);
  int index = -1;
  {
    const json apps = json::parse(slurp(env.apps_file));
    for (std::size_t i = 0; i < apps["apps"].size(); ++i)
      if (apps["apps"][i]["name"] == "星铁触屏") index = static_cast<int>(i);
  }
  ASSERT_GE(index, 0);
  const sparxie::Install install = install_of(kStarRail, "/x");
  ASSERT_TRUE(sparxie::prepare_launch(env, "星铁触屏", "星铁触屏", { 2868, 1320, 120 }).ok);
  EXPECT_EQ(read_values(install, reg).resolution, "1280×720");
  // the editor saves another size and window: the host reloads its application list for this request (apps_writes, read by the route), and the next launch reads the file again
  env.apps_writes = 0;
  const auto saved = sparxie::update_app(env, index, { { "display", { { "mode", "fixed" }, { "resolution", "1600×900" }, { "window", "fullscreen" } } } });
  ASSERT_TRUE(saved.status.ok) << saved.status.error;
  EXPECT_GT(env.apps_writes, 0) << "the route reloads the list when the file was written: no restart";
  ASSERT_TRUE(sparxie::prepare_launch(env, "星铁触屏", "星铁触屏", { 2868, 1320, 120 }).ok);
  const auto now = read_values(install, reg);
  EXPECT_EQ(now.resolution, "1600×900");
  EXPECT_EQ(now.display, "fullscreen");
  // a changed game setting of the preset (the item the person chose) reaches the game the same way
  env.apps_writes = 0;
  const auto vsync = sparxie::update_app(env, index, { { "settings", { { "vsync", true }, { "renderScale", 1.0 }, { "preset", "high" }, { "hdr", false }, { "display", "fullscreen" }, { "resolution", "1920×1080" }, { "dlss", "performance" } } } });
  ASSERT_TRUE(vsync.status.ok) << vsync.status.error;
  EXPECT_GT(env.apps_writes, 0);
  ASSERT_TRUE(sparxie::prepare_launch(env, "星铁触屏", "星铁触屏", { 2868, 1320, 120 }).ok);
  EXPECT_EQ(model(reg)["EnableVSync"], true);
  EXPECT_EQ(model(reg)["DlssQuality"], 2) << "DLSS Performance";
}

namespace {
  // a registry that stores nothing: the host must say so
  class RefusingRegistry: public sparxie::MemoryRegistry {
  public:
    bool
    set_dword(std::string_view, std::string_view, std::uint32_t) override { return false; }
    bool
    set_binary(std::string_view, std::string_view, const std::vector<std::uint8_t> &) override { return false; }
  };
}  // namespace

TEST(CapturedReal, ARegistryThatRefusesTheWriteIsNotReportedAsApplied) {
  const fs::path root = fresh("sparxie-captured-real-refused");
  sparxie::HostEnv env = env_at(root);
  RefusingRegistry reg;
  env.registry = &reg;
  env.display_size = [] { return std::pair<int, int> { 2868, 1320 }; };
  write_sample_apps(env);
  const auto started = sparxie::prepare_launch(env, "星铁触屏", "星铁触屏", { 2868, 1320, 120 });
  EXPECT_FALSE(started.ok);
  EXPECT_EQ(started.error_code, "registry_write_failed");
}

TEST(CapturedReal, AnApplicationThatHoldsNoChangedItemLeavesTheGamesOwnValues) {
  // 绝区零触屏 of the sample host after its file was converted to changed items only: nothing is held (before, it carried the seven default values with V-sync off that nobody picked); the game has V-sync on
  const fs::path root = fresh("sparxie-captured-real-seeded");
  sparxie::HostEnv env = env_at(root);
  sparxie::MemoryRegistry reg;
  env.registry = &reg;
  env.zzz_magics = { kMagic };
  env.display_size = [] { return std::pair<int, int> { 2868, 1320 }; };
  load_registry(reg, kZzz);
  write_sample_apps(env);
  const fs::path game = root / "game";
  place_zzz_file(game);
  json apps = json::parse(slurp(env.apps_file));
  for (auto &app : apps["apps"])
    if (app["name"] == "绝区零触屏") {
      EXPECT_EQ(app["sparxie"]["settings"].size(), 7u) << "the fixture is the file as it was before the conversion";
      app["sparxie"].erase("settingsMode");
      app["sparxie"]["settings"] = json::object();
      app["sparxie"]["dir"] = game.string();
    }
  std::ofstream(env.apps_file) << apps.dump(2);
  const sparxie::Install install = install_of(kZzz, game);
  EXPECT_EQ(read_values(install, reg).vsync, true);
  const std::string file_before = slurp(game / "ZenlessZoneZero_Data" / "Persistent" / "LocalStorage" / "GENERAL_DATA.bin");
  ASSERT_TRUE(sparxie::prepare_launch(env, "绝区零触屏", "绝区零触屏", { 2868, 1320, 120 }).ok);
  EXPECT_EQ(read_values(install, reg).vsync, true) << "the game's V-sync is not switched off by a default nobody chose";
  EXPECT_EQ(slurp(game / "ZenlessZoneZero_Data" / "Persistent" / "LocalStorage" / "GENERAL_DATA.bin"), file_before) << "the game's file is not rewritten";
  EXPECT_EQ(read_values(install, reg).resolution, "2868×1320") << "the display choice is still applied";
}
