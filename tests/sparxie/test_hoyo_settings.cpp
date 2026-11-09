#include "src/sparxie/hoyo_settings.h"
#include "src/sparxie/region.h"
#include "src/sparxie/settings.h"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

namespace fs = std::filesystem;

namespace {
  std::string
  root_of(const char *a, const char *b) { return std::string("Software") + static_cast<char>(92) + a + static_cast<char>(92) + b; }

  sparxie::json
  field_of(const sparxie::json &catalog, const char *game, const char *key) {
    for (const auto &title : catalog)
      if (title.value("game", std::string()) == game)
        for (const auto &item : title.value("settings", sparxie::json::array()))
          if (item.value("key", std::string()) == key) return item;
    return {};
  }

  void
  put_blob(sparxie::MemoryRegistry &reg, const std::string &root, const char *name, const std::string &text) {
    std::vector<std::uint8_t> bytes(text.begin(), text.end());
    bytes.push_back(0);
    reg.set_binary(root, name, bytes);
  }
}  // namespace

TEST(HoyoCatalog, GroupsAndStarRailFpsStaySeparateFromSparxie) {
  const sparxie::json catalog = sparxie::catalog_json(true);
  const char *games[] = { "genshin", "starrail", "zzz", "hi3" };
  for (const char *game : games) {
    std::set<std::string> sections;
    bool saw_volume = false;
    bool saw_args = false;
    for (const auto &title : catalog) {
      if (title.value("game", std::string()) != game) continue;
      for (const auto &item : title["settings"]) sections.insert(item.value("section", std::string()));
      for (const auto &item : title["sparxie"]["fields"]) EXPECT_NE(item.value("key", std::string()), "gameBoost");
      const auto fps = field_of(catalog, game, "fps");
      EXPECT_EQ(fps.value("section", std::string()), "display");
    }
    EXPECT_TRUE(sections.count("display"));
    EXPECT_TRUE(sections.count("quality"));
    EXPECT_TRUE(sections.count("audio"));
    EXPECT_TRUE(sections.count("language"));
    EXPECT_TRUE(sections.count("launch"));
    (void) saw_volume;
    (void) saw_args;
  }
  const auto rail = field_of(catalog, "starrail", "fps");
  bool has120 = false;
  for (const auto &option : rail["options"])
    if (option.value("value", 0) == 120) has120 = true;
  EXPECT_TRUE(has120);
  const auto sparxie = [&] {
    for (const auto &title : catalog)
      if (title.value("game", std::string()) == "starrail") return title["sparxie"]["fields"];
    return sparxie::json::array();
  }();
  bool sparxie_fps = false;
  for (const auto &item : sparxie)
    if (item.value("key", std::string()) == "fps" && item.value("section", std::string()) == "fps") sparxie_fps = true;
  EXPECT_TRUE(sparxie_fps);
}

TEST(HoyoSettings, GenshinGradesVolumesLanguageAndArgsRoundTrip) {
  sparxie::MemoryRegistry reg;
  sparxie::Install install;
  install.game = "genshin";
  install.region = "cn";
  install.id = "genshin-cn";
  const std::string root = root_of("miHoYo", "原神");
  const std::string perf = R"({"saveItems":[{"entryType":3,"index":3,"itemVersion":"CNRELWin5.0.0"},{"entryType":1,"index":1,"itemVersion":"CNRELWin5.0.0"}],"portedVersion":"CNRELWin5.0.0"})";
  sparxie::json general = { { "account", "keep-me" }, { "globalPerfData", perf }, { "volumeGlobal", 10 }, { "deviceLanguageType", 1 }, { "deviceVoiceLanguageType", 1 } };
  put_blob(reg, root, "GENERAL_DATA_h2389025596", general.dump());
  sparxie::GameValues values;
  values.extra = { { "shadow", "low" }, { "masterVolume", 4 }, { "textLanguage", "zh-cn" }, { "audioLanguage", "ja" }, { "customArgs", "-screen-quality 2" }, { "borderless", true }, { "graphicsApi", "12" }, { "gameBoost", true } };
  const std::set<std::string> fields = { "shadow", "masterVolume", "textLanguage", "audioLanguage", "customArgs", "borderless", "graphicsApi", "gameBoost" };
  auto wrote = sparxie::write_game_settings(install, reg, values, {}, [] { return false; }, {}, "restore", fields);
  EXPECT_TRUE(wrote.status.ok) << wrote.status.error;
  auto read = sparxie::read_game_settings(install, reg, {}, [] { return false; });
  EXPECT_EQ(read.values.extra.value("shadow", ""), "low");
  EXPECT_EQ(read.values.extra.value("masterVolume", 0), 4);
  EXPECT_EQ(read.values.extra.value("textLanguage", ""), "zh-cn");
  EXPECT_EQ(read.values.extra.value("audioLanguage", ""), "ja");
  EXPECT_EQ(read.values.extra.value("customArgs", ""), "-screen-quality 2");
  EXPECT_TRUE(read.values.extra.value("borderless", false));
  EXPECT_EQ(read.values.extra.value("graphicsApi", ""), "12");
  EXPECT_TRUE(read.values.extra.value("gameBoost", false));
  const auto blob = reg.get(root, "GENERAL_DATA_h2389025596");
  auto stored = sparxie::json::parse(std::string(blob.bytes.begin(), blob.bytes.end()));
  EXPECT_EQ(stored["account"], "keep-me");
  values.extra["customArgs"] = "calc.exe & whoami";
  auto bad = sparxie::write_game_settings(install, reg, values, {}, [] { return false; }, {}, "restore", { "customArgs" });
  EXPECT_EQ(bad.status.error_code, "invalid_input");
}

TEST(HoyoSettings, StarRailModelAudioLanguageRoundTrip) {
  sparxie::MemoryRegistry reg;
  sparxie::Install install;
  install.game = "starrail";
  install.region = "global";
  install.id = "starrail-global";
  const std::string root = root_of("Cognosphere", "Star Rail");
  put_blob(reg, root, "GraphicsSettings_Model_h2986158309", R"({"FPS":60,"EnableVSync":false,"RenderScale":1.2,"ShadowQuality":4,"AAMode":1,"DlssQuality":0,"EnableSelfShadow":2,"EnableHalfResTransparent":false})");
  reg.set_dword(root, "GraphicsSettings_GraphicsQuality_h523255858", 4);
  reg.set_dword(root, "AudioSettings_MasterVolume_h1622207037", 10);
  sparxie::GameValues values;
  values.fps = 120;
  values.extra = { { "shadow", "low" }, { "dlss", "quality" }, { "aa", "taa" }, { "selfShadow", true }, { "halfResTransparent", true }, { "masterVolume", 7 }, { "audioLanguage", "jp" }, { "textLanguage", "cht" } };
  const std::set<std::string> fields = { "fps", "shadow", "dlss", "aa", "selfShadow", "halfResTransparent", "masterVolume", "audioLanguage", "textLanguage" };
  auto wrote = sparxie::write_game_settings(install, reg, values, {}, [] { return false; }, {}, "restore", fields);
  EXPECT_TRUE(wrote.status.ok) << wrote.status.error;
  EXPECT_EQ(reg.get(root, "GraphicsSettings_GraphicsQuality_h523255858").dword, 0u);
  auto read = sparxie::read_game_settings(install, reg, {}, [] { return false; });
  EXPECT_EQ(read.values.fps, 120);
  EXPECT_EQ(read.values.extra.value("shadow", ""), "low");
  EXPECT_EQ(read.values.extra.value("dlss", ""), "quality");
  EXPECT_EQ(read.values.extra.value("aa", ""), "taa");
  EXPECT_TRUE(read.values.extra.value("selfShadow", false));
  EXPECT_TRUE(read.values.extra.value("halfResTransparent", false));
  EXPECT_EQ(read.values.extra.value("masterVolume", 0), 7);
  EXPECT_EQ(read.values.extra.value("audioLanguage", ""), "jp");
  EXPECT_EQ(read.values.extra.value("textLanguage", ""), "cht");
  EXPECT_TRUE(read.unavailable.empty());
}

TEST(HoyoSettings, HonkaiGraphicsAndAudioRoundTrip) {
  sparxie::MemoryRegistry reg;
  sparxie::Install install;
  install.game = "hi3";
  install.region = "cn";
  install.id = "hi3-cn";
  const std::string root = root_of("miHoYo", "崩坏3");
  put_blob(reg, root, "GENERAL_DATA_V2_PersonalGraphicsSettingV2_h3480068519", R"({"ResolutionQuality":"Middle","ShadowLevel":"MIDDLE","TargetFrameRateForInLevel":60,"TargetFrameRateForOthers":60,"AAType":"FXAA","UseHDR":true,"LodGrade":1})");
  put_blob(reg, root, "GENERAL_DATA_V2_PersonalAudioSetting_h3869048096", R"({"MasterVolume":100,"BGMVolume":100,"_userCVLanguage":"Japanese","CVLanguage":"Japanese"})");
  sparxie::GameValues values;
  values.fps = 30;
  values.extra = { { "shadow", "high" }, { "resolutionQuality", "1.0" }, { "fpsMenu", 30 }, { "useHdr", false }, { "masterVolume", 40 }, { "audioLanguage", "Chinese(PRC)" } };
  const std::set<std::string> fields = { "fps", "shadow", "resolutionQuality", "fpsMenu", "useHdr", "masterVolume", "audioLanguage" };
  auto wrote = sparxie::write_game_settings(install, reg, values, {}, [] { return false; }, {}, "restore", fields);
  EXPECT_TRUE(wrote.status.ok) << wrote.status.error;
  auto read = sparxie::read_game_settings(install, reg, {}, [] { return false; });
  EXPECT_EQ(read.values.fps, 30);
  EXPECT_EQ(read.values.extra.value("shadow", ""), "high");
  EXPECT_EQ(read.values.extra.value("resolutionQuality", ""), "1.0");
  EXPECT_EQ(read.values.extra.value("fpsMenu", 0), 30);
  EXPECT_FALSE(read.values.extra.value("useHdr", true));
  EXPECT_EQ(read.values.extra.value("masterVolume", 0), 40);
  EXPECT_EQ(read.values.extra.value("audioLanguage", ""), "Chinese(PRC)");
  const auto vol = reg.get(root, "GENERAL_DATA_V2_PersonalAudioSettingVolume_h600615720");
  auto floats = sparxie::json::parse(std::string(vol.bytes.begin(), vol.bytes.end()));
  EXPECT_NEAR(floats.value("MasterVolumeValue", 0.0), 40.0, 0.01);
}

TEST(HoyoSettings, ZenlessFileFieldsRoundTripAndRefuseUndecodable) {
  std::error_code ec;
  fs::path base = "/scratch/clp-hoyo-tests";
  if (!fs::is_directory("/scratch", ec)) base = fs::temp_directory_path() / "clp-hoyo-tests";
  const fs::path dir = base / "zzz";
  fs::remove_all(dir);
  const fs::path file = dir / "ZenlessZoneZero_Data" / "Persistent" / "LocalStorage" / "GENERAL_DATA.bin";
  fs::create_directories(file.parent_path());
  const std::string text = R"({"$Type":"MoleMole.GeneralLocalDataItem","SystemSettingDataMap":{"3":{"$Type":"MoleMole.SystemSettingLocalData","Version":0,"Data":1},"8":{"$Type":"MoleMole.SystemSettingLocalData","Version":0,"Data":1},"9":{"$Type":"MoleMole.SystemSettingLocalData","Version":0,"Data":1},"10":{"$Type":"MoleMole.SystemSettingLocalData","Version":0,"Data":1},"31":{"$Type":"MoleMole.SystemSettingLocalData","Version":0,"Data":10}},"HDRSettingRecordState":0,"DeviceLanguageType":1,"DeviceLanguageVoiceType":2})";
  const std::vector<std::uint8_t> magic { 0x11, 0x22, 0x33, 0x44 };
  const auto encoded = sparxie::sleepy_write(text, magic);
  {
    std::ofstream out(file, std::ios::binary);
    out.write(reinterpret_cast<const char *>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
  }
  sparxie::MemoryRegistry reg;
  sparxie::Install install;
  install.game = "zzz";
  install.region = "cn";
  install.id = "zzz-cn";
  install.dir = dir.string();
  sparxie::GameValues values;
  values.extra = { { "shadow", "high" }, { "masterVolume", 6 }, { "textLanguage", "zh-cn" }, { "audioLanguage", "ja" }, { "customResolution", true }, { "customWidth", 2560 }, { "customHeight", 1440 } };
  const std::set<std::string> fields = { "shadow", "masterVolume", "textLanguage", "audioLanguage", "customResolution", "customWidth", "customHeight" };
  auto wrote = sparxie::write_game_settings(install, reg, values, { magic }, [] { return false; }, {}, "restore", fields);
  EXPECT_TRUE(wrote.status.ok) << wrote.status.error;
  auto read = sparxie::read_game_settings(install, reg, { magic }, [] { return false; });
  EXPECT_EQ(read.values.extra.value("shadow", ""), "high");
  EXPECT_EQ(read.values.extra.value("masterVolume", 0), 6);
  EXPECT_EQ(read.values.extra.value("textLanguage", ""), "zh-cn");
  EXPECT_EQ(read.values.extra.value("audioLanguage", ""), "ja");
  EXPECT_TRUE(read.values.extra.value("customResolution", false));
  EXPECT_EQ(reg.get(root_of("miHoYo", "绝区零"), "Screenmanager Resolution Width_h182942802").dword, 2560u);
  const std::vector<std::uint8_t> wrong { 0x01 };
  auto hidden = sparxie::read_game_settings(install, reg, { wrong }, [] { return false; });
  EXPECT_NE(std::find(hidden.unavailable.begin(), hidden.unavailable.end(), "shadow"), hidden.unavailable.end());
  std::ifstream in(file, std::ios::binary);
  const std::vector<std::uint8_t> still((std::istreambuf_iterator<char>(in)), {});
  const std::string decoded = sparxie::sleepy_read(still, magic);
  EXPECT_NE(decoded.find("\"10\":{\"$Type\":\"MoleMole.SystemSettingLocalData\",\"Version\":0,\"Data\":2}"), std::string::npos) << decoded;
}

TEST(HoyoLaunch, UnityArgumentsFollowCollapse) {
  sparxie::Install genshin;
  genshin.game = "genshin";
  sparxie::json options = { { "exclusiveFullscreen", true }, { "borderless", true }, { "resolution", "1920x1080" }, { "customArgs", "-force-d3d11" }, { "graphicsApi", "12" } };
  const std::string command = sparxie::game_launch_command("game.exe", genshin, options);
  EXPECT_NE(command.find("-window-mode exclusive"), std::string::npos);
  EXPECT_NE(command.find("-screen-fullscreen 1"), std::string::npos);
  EXPECT_NE(command.find("-screen-width 1920"), std::string::npos);
  EXPECT_NE(command.find("-screen-height 1080"), std::string::npos);
  EXPECT_NE(command.find("-popupwindow"), std::string::npos);
  EXPECT_NE(command.find("\"-force-d3d11\""), std::string::npos);
  EXPECT_EQ(command.find("calc"), std::string::npos);

  sparxie::Install hi3;
  hi3.game = "hi3";
  const std::string hi = sparxie::game_launch_command("game.exe", hi3, { { "graphicsApi", "11.1ns" }, { "resolution", "1280x720" } });
  EXPECT_NE(hi.find("-force-feature-level-11-1 -force-d3d11-no-singlethreaded"), std::string::npos);
  EXPECT_NE(hi.find("-screen-width 1280"), std::string::npos);

  sparxie::Install zzz;
  zzz.game = "zzz";
  const std::string plain = sparxie::game_launch_command("game.exe", zzz, { { "resolution", "1920x1080" }, { "graphicsApi", "12" } });
  EXPECT_EQ(plain.find("-screen-width"), std::string::npos);
  EXPECT_NE(plain.find("-use-d3d12"), std::string::npos);
  const std::string custom = sparxie::game_launch_command("game.exe", zzz, { { "customResolution", true }, { "customWidth", 1600 }, { "customHeight", 900 } });
  EXPECT_NE(custom.find("-screen-width 1600"), std::string::npos);
  EXPECT_NE(custom.find("-screen-height 900"), std::string::npos);
}
