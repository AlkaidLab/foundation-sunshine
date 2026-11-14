#include "src/sparxie/host.h"
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
namespace fs = std::filesystem;
namespace {
  // What a captured apps.json held after the generic apps writer (boost property_tree) saved it: every scalar of the sparxie block is a string, empty arrays and objects are "".
  const char *kStringified = R"({
    "env": { "SUNSHINE_APP_STATUS": "IDLE" },
    "apps": [
      { "name": "绝区零触屏", "cmd": "\"C:\\Program Files\\Sunshine\\tools\\sparxie-launch.exe\" --profile zzz-cn-1", "elevated": "true",
        "sparxie": { "game": "zzz", "region": "cn", "install": "zzz-cn", "profile": "zzz-cn-1", "mode": "touch", "cover": "official", "restore": "restore", "settingsMode": "preset",
          "artVersion": "12345678", "launchTemplateProvisional": "true", "candidates": "", "markers": ["exe", "registry"],
          "settings": { "resolution": "1920×1080", "display": "windowed", "fps": "60", "vsync": "false", "renderScale": "1.2", "preset": "medium", "hdr": "false" },
          "options": { "bgFps": "10", "bgLimit": "true", "followPreset": "false", "fps": "120", "fpsUnlock": "false", "input": "kbm", "isDefault": true, "preset30": "60", "preset60": "1000", "priority": "normal", "scale": "400", "scaleOverride": "false" } } },
      { "name": "原神", "sparxie": { "game": "genshin", "mode": "touch", "settings": "", "options": { "fps": "display", "fpsUnlock": "true" } } },
      { "name": "Desktop", "image-path": "desktop.png" }
    ] })";
  fs::path
  scratch(const char *name) {
    const fs::path root = fs::temp_directory_path() / name;
    fs::remove_all(root);
    fs::create_directories(root);
    return root;
  }
}  // namespace

TEST(AppsTypes, StringifiedBlockGetsItsTypesBack) {
  sparxie::json apps = sparxie::json::parse(kStringified);
  EXPECT_TRUE(sparxie::normalize_apps(apps));
  const auto &block = apps["apps"][0]["sparxie"];
  EXPECT_TRUE(block["launchTemplateProvisional"].is_boolean());
  EXPECT_TRUE(block["candidates"].is_array());
  EXPECT_EQ(block["markers"].size(), 2u);
  EXPECT_EQ(block["artVersion"], "12345678") << "a version that looks like a number stays text";
  EXPECT_EQ(block["settings"]["fps"], 60);
  EXPECT_EQ(block["settings"]["vsync"], false);
  EXPECT_EQ(block["settings"]["hdr"], false);
  EXPECT_DOUBLE_EQ(block["settings"]["renderScale"].get<double>(), 1.2);
  EXPECT_EQ(block["settings"]["resolution"], "1920×1080");
  EXPECT_EQ(block["settings"]["preset"], "medium");
  const auto &options = block["options"];
  EXPECT_EQ(options["fps"], 120);
  EXPECT_EQ(options["bgFps"], 10);
  EXPECT_EQ(options["preset30"], 60);
  EXPECT_EQ(options["scale"], 400);
  EXPECT_EQ(options["fpsUnlock"], false);
  EXPECT_EQ(options["bgLimit"], true);
  EXPECT_EQ(options["followPreset"], false);
  EXPECT_EQ(options["scaleOverride"], false);
  EXPECT_EQ(options["isDefault"], true) << "a typed value is left alone";
  EXPECT_EQ(options["input"], "kbm");
  EXPECT_EQ(options["priority"], "normal");
  const auto &second = apps["apps"][1]["sparxie"];
  EXPECT_TRUE(second["settings"].is_object());
  EXPECT_EQ(second["options"]["fps"], "display") << "the tier that follows the display stays a word";
  EXPECT_EQ(second["options"]["fpsUnlock"], true);
  EXPECT_EQ(apps["apps"][0]["elevated"], "true") << "only the sparxie block is touched";
  EXPECT_FALSE(sparxie::normalize_apps(apps)) << "a second pass changes nothing";
}

TEST(AppsTypes, TheTextAndTheFileHeal) {
  const std::string typed = sparxie::typed_apps_text(kStringified);
  EXPECT_NE(typed, kStringified);
  const auto parsed = sparxie::json::parse(typed);
  EXPECT_EQ(parsed["apps"][0]["sparxie"]["options"]["fpsUnlock"], false);
  EXPECT_EQ(sparxie::typed_apps_text(typed), typed) << "typed text comes back unchanged";
  const std::string plain = R"({"apps":[{"name":"Desktop"}]})";
  EXPECT_EQ(sparxie::typed_apps_text(plain), plain);
  EXPECT_EQ(sparxie::typed_apps_text("not json \"sparxie\""), "not json \"sparxie\"");

  const fs::path root = scratch("sparxie-apps-types");
  const fs::path file = root / "apps.json";
  {
    std::ofstream out(file, std::ios::binary);
    out << kStringified;
  }
  sparxie::heal_apps_file(file.string());
  std::ifstream in(file);
  sparxie::json healed;
  in >> healed;
  EXPECT_EQ(healed["apps"][0]["sparxie"]["settings"]["vsync"], false);
  EXPECT_EQ(healed["apps"][2]["name"], "Desktop");
  EXPECT_FALSE(fs::exists(root / "apps.json.sparxie-tmp"));
}

TEST(AppsTypes, TheHostReadsAStringifiedFileAndSavesItTyped) {
  const fs::path root = scratch("sparxie-apps-read");
  sparxie::HostEnv env;
  env.host_data = root / "host";
  env.data_dir = root / "sparxie-data";
  env.apps_file = root / "apps.json";
  env.covers_dir = root / "covers";
  env.install_root = root / "install";
  {
    std::ofstream out(env.apps_file, std::ios::binary);
    out << kStringified;
  }
  // the editor's save (PUT) reads the app as what it is, changes one thing, and writes the whole block typed
  const auto saved = sparxie::update_app(env, 0, { { "name", "绝区零触屏" }, { "mode", "pc" }, { "settings", { { "fps", 30 }, { "vsync", true }, { "renderScale", 0.8 }, { "preset", "low" }, { "hdr", true } } } });
  ASSERT_TRUE(saved.status.ok) << saved.status.error;
  std::ifstream in(env.apps_file);
  sparxie::json apps;
  in >> apps;
  const auto &block = apps["apps"][0]["sparxie"];
  EXPECT_EQ(block["settings"]["fps"], 30);
  EXPECT_EQ(block["settings"]["vsync"], true);
  EXPECT_EQ(block["options"]["fpsUnlock"], false) << "the options that were strings are booleans and numbers in the file again";
  EXPECT_EQ(block["options"]["fps"], 120);
  EXPECT_EQ(block["options"]["bgFps"], 10);
  EXPECT_TRUE(block["launchTemplateProvisional"].is_boolean());
  EXPECT_TRUE(apps["apps"][1]["sparxie"]["settings"].is_object());
}
