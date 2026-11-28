#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include "src/sparxie/host.h"
#include "src/sparxie/sha256.h"
#include "src/sparxie/sparxie_build.h"
#include "src/sparxie/store.h"
namespace fs = std::filesystem;
using sparxie::json;

// The two Sparxie generations (docs/sparxie-api.md, "Two Sparxie generations"). The first ships Endfield in the build ("endfield" in config.json); the second
// reads it as the plugin ext.skyhua0224.endfield and sets the whole config aside when a profile still says "endfield". The host writes what the installed build reads.
namespace {
  constexpr const char *kPluginDir = "plugins/ext.skyhua0224.endfield";
  // The plugin.json of the real plugin, cut to the parts the host reads: id, executables and the settings it declares
  const char *kPluginJson = R"({
    "id": "ext.skyhua0224.endfield", "displayName": "明日方舟：终末地", "abiMajor": 1, "exeNames": ["Endfield.exe"], "version": "0.1.0",
    "settingsSchema": { "fields": [
      { "key": "inputMode", "label": "界面模式", "type": "enum", "default": "touch", "options": [ { "value": "touch", "label": "触屏" }, { "value": "pc", "label": "键鼠" } ] },
      { "key": "autoResolution", "label": "r", "type": "bool", "default": true },
      { "key": "width", "label": "w", "type": "int", "min": 320, "max": 7680, "default": 1920 },
      { "key": "height", "label": "h", "type": "int", "min": 240, "max": 4320, "default": 1080 },
      { "key": "refreshRate", "label": "f", "type": "int", "min": 24, "max": 1000, "default": 60 },
      { "key": "fpsUnlock", "label": "u", "type": "bool", "default": true },
      { "key": "targetFps", "label": "t", "type": "int", "min": 10, "max": 1000, "default": 120 },
      { "key": "uiScalePercent", "label": "s", "type": "int", "min": 100, "max": 180, "step": 5, "default": 140 } ] } })";

  void
  write(const fs::path &path, const std::string &text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
  }
  fs::path
  fresh(const char *name) {
    const fs::path root = fs::temp_directory_path() / name;
    fs::remove_all(root);
    fs::create_directories(root);
    return root;
  }
  // the install of the second generation: the Contracts of that build name GameIds and settings_json
  void
  plugin_era_install(const fs::path &dir) {
    write(dir / "Sparxie.exe", "x");
    write(dir / "version.ini", "exe_path=app-0.1.0-dev.test\\Sparxie.Desktop.exe\n");
    write(dir / "app-0.1.0-dev.test" / "Sparxie.Contracts.dll", std::string("MZ\0\0", 4) + "GameIds" + std::string(1, '\0') + "SettingsJson" + std::string(1, '\0'));
  }
  // the first generation (preview.9-sky.11): the protocol of that build has no GameIds; it lists Endfield in capabilities.json
  void
  legacy_install(const fs::path &dir, bool with_endfield) {
    write(dir / "Sparxie.exe", "x");
    write(dir / "version.ini", "exe_path=app-0.1.0-test\\Sparxie.Desktop.exe\n");
    write(dir / "app-0.1.0-test" / "Sparxie.Contracts.dll", std::string("MZ\0\0", 4) + "ScreenArgs" + std::string(1, '\0') + "TouchUiEnabled" + std::string(1, '\0'));
    write(dir / "app-0.1.0-test" / "capabilities.json", with_endfield ? R"({"schema":1,"capabilities":["endfield","screen-args","touch-ui-switch"]})" : R"({"schema":1,"capabilities":["screen-args"]})");
  }
  void
  endfield_plugin(const fs::path &dir, bool trusted, const std::string &plugin_json = kPluginJson) {
    write(dir / kPluginDir / "plugin.json", plugin_json);
    write(dir / kPluginDir / "module.dll", "MZ the module");
    if (!trusted) return;
    json trust = { { "version", 1 }, { "records", json::array({ { { "id", "ext.skyhua0224.endfield" },
                     { "dllSha256", sparxie::sha256::hash(std::string("MZ the module")) },
                     { "manifestSha256", sparxie::sha256::hash(plugin_json) }, { "size", 13 } } }) } };
    write(dir / "plugin-trust.json", trust.dump());
  }
  sparxie::ProfileDraft
  endfield_draft(const std::string &resolution = "auto", const std::string &input = "touch") {
    sparxie::ProfileDraft draft;
    draft.id = "endfield-cn-1";
    draft.game = "endfield";
    draft.display_name = "终末地";
    draft.executable = "C:/Games/Arknights Endfield/Endfield.exe";
    draft.options = { { "input", input }, { "fps", 144 }, { "fpsUnlock", true }, { "resolution", resolution } };
    draft.display_fps = 120;
    return draft;
  }
  json
  stored(const fs::path &dir) { return sparxie::read_store(dir).document; }
}  // namespace

TEST(SparxieGenerations, TheBuildIsLearnedFromItsFilesNotFromAVersion) {
  const fs::path dir = fresh("sparxie-gen-detect");
  EXPECT_EQ(sparxie::detect_build(dir).generation, sparxie::Generation::unknown) << "no install: nothing reads the config yet";
  plugin_era_install(dir);
  EXPECT_EQ(sparxie::detect_build(dir).generation, sparxie::Generation::plugin);
  // one marker alone does not make the second generation
  write(dir / "app-0.1.0-dev.test" / "Sparxie.Contracts.dll", "MZ GameIds");
  EXPECT_EQ(sparxie::detect_build(dir).generation, sparxie::Generation::legacy);

  const fs::path old = fresh("sparxie-gen-detect-old");
  legacy_install(old, true);
  const auto build = sparxie::detect_build(old);
  EXPECT_EQ(build.generation, sparxie::Generation::legacy);
  EXPECT_TRUE(build.builtin_endfield);
  EXPECT_EQ(sparxie::endfield_support(build), sparxie::EndfieldSupport::builtin);
  legacy_install(old, false);
  EXPECT_FALSE(sparxie::detect_build(old).builtin_endfield);
  EXPECT_EQ(sparxie::endfield_support(sparxie::detect_build(old)), sparxie::EndfieldSupport::unsupported);
  // a build that ships the two Endfield libraries but no list
  legacy_install(old, false);
  write(old / "app-0.1.0-test" / "EndfieldCore.dll", "x");
  EXPECT_FALSE(sparxie::detect_build(old).builtin_endfield) << "one library is not Endfield";
  write(old / "app-0.1.0-test" / "EndfieldRuntime.dll", "x");
  EXPECT_TRUE(sparxie::detect_build(old).builtin_endfield);
}

TEST(SparxieGenerations, TheVersionIniNamesTheBuildThatRuns) {
  const fs::path dir = fresh("sparxie-gen-versionini");
  legacy_install(dir, true);
  plugin_era_install(dir);  // an older app folder stays beside the newer one after an update
  write(dir / "version.ini", "exe_path=app-0.1.0-test\\Sparxie.Desktop.exe\n");
  EXPECT_EQ(sparxie::detect_build(dir).generation, sparxie::Generation::legacy) << "version.ini wins over the folder names";
  write(dir / "version.ini", "exe_path=app-0.1.0-dev.test\\Sparxie.Desktop.exe\n");
  EXPECT_EQ(sparxie::detect_build(dir).generation, sparxie::Generation::plugin);
  fs::remove(dir / "version.ini");
  EXPECT_EQ(sparxie::detect_build(dir).generation, sparxie::Generation::legacy) << "without it the newest folder name is taken: app-0.1.0-test > app-0.1.0-dev.test";
}

TEST(SparxieGenerations, ThePluginIsInstalledOnlyWhenItsFilesAreRight) {
  const fs::path dir = fresh("sparxie-gen-plugin");
  plugin_era_install(dir);
  EXPECT_FALSE(sparxie::detect_build(dir).endfield.installed);
  EXPECT_EQ(sparxie::endfield_support(sparxie::detect_build(dir)), sparxie::EndfieldSupport::plugin_missing);

  endfield_plugin(dir, false);
  auto build = sparxie::detect_build(dir);
  EXPECT_TRUE(build.endfield.installed);
  EXPECT_FALSE(build.endfield.trusted) << "dropped into the folder, not yet enabled in Sparxie";
  EXPECT_EQ(build.endfield.version, "0.1.0");
  EXPECT_EQ(build.endfield.keys.size(), 8u);
  EXPECT_EQ(sparxie::endfield_support(build), sparxie::EndfieldSupport::plugin);

  endfield_plugin(dir, true);
  EXPECT_TRUE(sparxie::detect_build(dir).endfield.trusted);
  write(dir / kPluginDir / "module.dll", "MZ another module");
  EXPECT_FALSE(sparxie::detect_build(dir).endfield.trusted) << "a replaced module is no longer the one that was confirmed";
  EXPECT_TRUE(sparxie::detect_build(dir).endfield.installed);

  fs::remove(dir / kPluginDir / "module.dll");
  EXPECT_FALSE(sparxie::detect_build(dir).endfield.installed) << "no module.dll";
  endfield_plugin(dir, false, R"({"id":"ext.someone.else","exeNames":["Endfield.exe"]})");
  EXPECT_FALSE(sparxie::detect_build(dir).endfield.installed) << "another id in the folder of this one";
  endfield_plugin(dir, false, R"({"id":"ext.skyhua0224.endfield","exeNames":["Other.exe"]})");
  EXPECT_FALSE(sparxie::detect_build(dir).endfield.installed) << "not a plugin of Endfield.exe";
  endfield_plugin(dir, false, "{ not json");
  EXPECT_FALSE(sparxie::detect_build(dir).endfield.installed);
}

TEST(SparxieGenerations, TheIdSyntaxIsSparxiesOwn) {
  for (const char *good : { "ext.skyhua0224.endfield", "ext.ab.cd", "ext.a1-b2.c3-d4" }) EXPECT_TRUE(sparxie::is_plugin_game_id(good)) << good;
  for (const char *bad : { "endfield", "genshin", "ext.a.endfield", "ext.vendor.", "Ext.vendor.game", "ext.vendor.game.extra", "ext.vendor.Game", "ext..game", "", "ext." })
    EXPECT_FALSE(sparxie::is_plugin_game_id(bad)) << bad;
  EXPECT_EQ(sparxie::from_sparxie_game("ext.skyhua0224.endfield"), "endfield");
  EXPECT_EQ(sparxie::from_sparxie_game("endfield"), "endfield");
  EXPECT_EQ(sparxie::from_sparxie_game("ext.someone.else"), "") << "a plugin the host knows nothing about is no game of its catalog";
}

TEST(SparxieGenerations, TheSecondGenerationGetsThePluginsIdAndSettings) {
  const fs::path dir = fresh("sparxie-gen-write-plugin");
  plugin_era_install(dir);
  endfield_plugin(dir, true);
  const auto wrote = sparxie::add_profile(dir, endfield_draft("1920×1080", "kbm"), {});
  ASSERT_TRUE(wrote.status.ok) << wrote.status.error;
  const json doc = stored(dir);
  const json &profile = doc["profiles"][0];
  EXPECT_EQ(profile["game"], "ext.skyhua0224.endfield");
  EXPECT_FALSE(profile.contains("hoyo")) << "a plugin game has no hoyo block";
  ASSERT_TRUE(profile["settings"].is_object());
  EXPECT_EQ(profile["settings"]["inputMode"], "pc");
  EXPECT_EQ(profile["settings"]["autoResolution"], false);
  EXPECT_EQ(profile["settings"]["width"], 1920);
  EXPECT_EQ(profile["settings"]["height"], 1080);
  EXPECT_EQ(profile["settings"]["refreshRate"], 120);
  EXPECT_EQ(profile["settings"]["fpsUnlock"], true);
  EXPECT_EQ(profile["settings"]["targetFps"], 144);
  EXPECT_EQ(profile["executablePath"], "C:/Games/Arknights Endfield/Endfield.exe");
  EXPECT_EQ(sparxie::read_store(dir).state, "data");
  // read back as the application's own options
  const json options = sparxie::options_from_profile(profile);
  EXPECT_EQ(options["input"], "kbm");
  EXPECT_EQ(options["resolution"], "1920×1080");
  EXPECT_EQ(options["fps"], 144);
  EXPECT_EQ(options["fpsUnlock"], true);
  // the stream's size: the profile follows it and keeps a size Sparxie would accept
  sparxie::ProfileDraft follow = endfield_draft("auto", "touch");
  ASSERT_TRUE(sparxie::update_profile(dir, "endfield-cn-1", follow, {}).status.ok);
  const json auto_doc = stored(dir);
  const json &auto_profile = auto_doc["profiles"][0];
  EXPECT_EQ(auto_profile["settings"]["autoResolution"], true);
  EXPECT_EQ(auto_profile["settings"]["width"], 1920) << "the size that was there stays while the size follows the stream";
  EXPECT_EQ(auto_profile["settings"]["inputMode"], "touch");
  EXPECT_FALSE(sparxie::options_from_profile(auto_profile).contains("resolution"));
}

TEST(SparxieGenerations, TheSettingsTheHostDoesNotManageSurviveAnUpdate) {
  const fs::path dir = fresh("sparxie-gen-keep-settings");
  plugin_era_install(dir);
  endfield_plugin(dir, true);
  ASSERT_TRUE(sparxie::add_profile(dir, endfield_draft(), {}).status.ok);
  json doc = stored(dir);
  doc["profiles"][0]["settings"]["uiScalePercent"] = 165;  // the person's own, set in Sparxie
  doc["profiles"][0]["future"] = 1;
  write(dir / "config.json", doc.dump());
  sparxie::ProfileDraft draft = endfield_draft("auto", "kbm");
  draft.options["fps"] = 90;
  ASSERT_TRUE(sparxie::update_profile(dir, "endfield-cn-1", draft, {}).status.ok);
  const json kept_doc = stored(dir);
  const json &profile = kept_doc["profiles"][0];
  EXPECT_EQ(profile["settings"]["uiScalePercent"], 165);
  EXPECT_EQ(profile["future"], 1);
  EXPECT_EQ(profile["settings"]["inputMode"], "pc");
  EXPECT_EQ(profile["settings"]["targetFps"], 90);
}

TEST(SparxieGenerations, OnlyTheKeysThePluginDeclaresAreWritten) {
  const fs::path dir = fresh("sparxie-gen-schema-keys");
  plugin_era_install(dir);
  endfield_plugin(dir, true, R"({"id":"ext.skyhua0224.endfield","exeNames":["Endfield.exe"],"settingsSchema":{"fields":[
    {"key":"inputMode","label":"i","type":"enum","default":"touch","options":[{"value":"touch","label":"t"},{"value":"pc","label":"p"}]},
    {"key":"targetFps","label":"t","type":"int","min":10,"max":1000,"default":120}]}})");
  ASSERT_TRUE(sparxie::add_profile(dir, endfield_draft(), {}).status.ok);
  const json keys_doc = stored(dir);
  const json &settings = keys_doc["profiles"][0]["settings"];
  EXPECT_EQ(settings.size(), 2u) << settings.dump();
  EXPECT_TRUE(settings.contains("inputMode") && settings.contains("targetFps"));
}

TEST(SparxieGenerations, WithoutThePluginNoEndfieldProfileIsMadeAndNothingIsTouched) {
  const fs::path dir = fresh("sparxie-gen-no-plugin");
  plugin_era_install(dir);
  const std::string before = R"({"schemaVersion":1,"selectedProfileId":"keep","profiles":[{"id":"keep","displayName":"星铁","game":"starRail","executablePath":"C:/S/StarRail.exe","hoyo":{"fpsUnlockEnabled":true,"targetFps":120}}]})";
  write(dir / "config.json", before);
  const auto refused = sparxie::add_profile(dir, endfield_draft(), {});
  EXPECT_FALSE(refused.status.ok);
  EXPECT_EQ(refused.status.error_code, "endfield_plugin_missing");
  EXPECT_EQ(refused.status.http, 409);
  EXPECT_EQ(refused.state, "refused_endfield");
  EXPECT_NE(refused.status.error.find("需要终末地插件"), std::string::npos);
  std::ifstream in(dir / "config.json");
  EXPECT_EQ(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()), before) << "the file is byte for byte what it was";
  for (const auto &entry : fs::directory_iterator(dir)) EXPECT_NE(entry.path().filename().string().rfind("config.backup-", 0), 0u) << "no backup for a write that did not happen";
  // the other titles are still written, in their own spelling
  sparxie::ProfileDraft genshin;
  genshin.game = "genshin";
  genshin.display_name = "原神";
  genshin.executable = "C:/G/YuanShen.exe";
  ASSERT_TRUE(sparxie::add_profile(dir, genshin, {}).status.ok);
  EXPECT_EQ(stored(dir)["profiles"][1]["game"], "genshin");
  // an update of an Endfield profile is refused the same way
  EXPECT_EQ(sparxie::update_profile(dir, "keep", endfield_draft(), {}).status.error_code, "endfield_plugin_missing");
}

TEST(SparxieGenerations, TheFirstGenerationKeepsItsOwnSpelling) {
  const fs::path dir = fresh("sparxie-gen-write-legacy");
  legacy_install(dir, true);
  ASSERT_TRUE(sparxie::add_profile(dir, endfield_draft("1920×1080", "kbm"), {}).status.ok);
  const json legacy_doc = stored(dir);
  const json &profile = legacy_doc["profiles"][0];
  EXPECT_EQ(profile["game"], "endfield");
  EXPECT_EQ(profile["hoyo"]["endfieldInputMode"], "pc");
  EXPECT_EQ(profile["hoyo"]["endfieldWidth"], 1920);
  EXPECT_FALSE(profile.contains("settings"));
  EXPECT_EQ(sparxie::read_store(dir).state, "data");

  const fs::path bare = fresh("sparxie-gen-write-legacy-bare");
  legacy_install(bare, false);
  const auto refused = sparxie::add_profile(bare, endfield_draft(), {});
  EXPECT_EQ(refused.status.error_code, "endfield_unsupported") << "a build that cannot read the id would put the whole file aside";
  EXPECT_FALSE(fs::exists(bare / "config.json"));
}

TEST(SparxieGenerations, ABuildThatCannotBeFoundKeepsTheFirstSpelling) {
  const fs::path dir = fresh("sparxie-gen-unknown");
  ASSERT_TRUE(sparxie::add_profile(dir, endfield_draft(), {}).status.ok);
  EXPECT_EQ(stored(dir)["profiles"][0]["game"], "endfield");
  // tests (and a host without the component) can say which build the write is for
  sparxie::StoreHooks hooks;
  sparxie::SparxieBuild build;
  build.generation = sparxie::Generation::plugin;
  build.endfield.installed = true;
  hooks.build = build;
  const fs::path other = fresh("sparxie-gen-hooks");
  ASSERT_TRUE(sparxie::add_profile(other, endfield_draft(), hooks).status.ok);
  EXPECT_EQ(stored(other)["profiles"][0]["game"], "ext.skyhua0224.endfield");
}

TEST(SparxieGenerations, AnEndfieldProfileOfTheFirstSpellingIsRepairedWhenThePluginIsThere) {
  // 2026-11-23: the owner's config held two "endfield" profiles; the new Sparxie put the whole file aside
  const fs::path dir = fresh("sparxie-gen-repair");
  plugin_era_install(dir);
  endfield_plugin(dir, true);
  const json hoyo_a = { { "fpsUnlockEnabled", true }, { "targetFps", 120 }, { "backgroundFpsLimitEnabled", true }, { "backgroundFps", 10 }, { "processPriority", "normal" }, { "endfieldInputMode", "touch" },
                        { "endfieldAutoResolution", true }, { "endfieldWidth", 1920 }, { "endfieldHeight", 1080 }, { "endfieldRefreshRate", 60 } };
  json hoyo_b = hoyo_a;
  hoyo_b["endfieldInputMode"] = "pc";
  hoyo_b["endfieldAutoResolution"] = false;
  hoyo_b["endfieldWidth"] = 2560;
  hoyo_b["endfieldHeight"] = 1440;
  hoyo_b["endfieldRefreshRate"] = 144;
  hoyo_b["fpsUnlockEnabled"] = false;
  const json doc = { { "schemaVersion", 1 }, { "selectedProfileId", "endfield-cn-1" }, { "profiles", json::array({
    { { "id", "endfield-cn-1" }, { "displayName", "终末地触屏" }, { "game", "endfield" }, { "executablePath", "C:/G/Endfield.exe" }, { "hoyo", hoyo_a } },
    { { "id", "endfield-cn-2" }, { "displayName", "终末地键鼠" }, { "game", "endfield" }, { "executablePath", "C:/G/Endfield.exe" }, { "hoyo", hoyo_b } },
    { { "id", "genshin-cn-1" }, { "displayName", "原神" }, { "game", "genshin" }, { "executablePath", "C:/G/YuanShen.exe" } } }) } };
  write(dir / "config.json", doc.dump());
  EXPECT_EQ(sparxie::read_store(dir).state, "unreadable") << "as read by the second generation the file is invalid; that is what made Sparxie set it aside";
  sparxie::ProfileDraft genshin;
  genshin.game = "genshin";
  genshin.display_name = "新的原神";
  genshin.executable = "C:/G/YuanShen.exe";
  ASSERT_TRUE(sparxie::add_profile(dir, genshin, {}).status.ok) << "any write of the host repairs what an earlier host left";
  const json after = stored(dir);
  ASSERT_EQ(after["profiles"].size(), 4u);
  const json &touch = after["profiles"][0];
  EXPECT_EQ(touch["game"], "ext.skyhua0224.endfield");
  EXPECT_FALSE(touch.contains("hoyo"));
  EXPECT_EQ(touch["settings"]["inputMode"], "touch");
  EXPECT_EQ(touch["settings"]["autoResolution"], true);
  EXPECT_EQ(touch["settings"]["fpsUnlock"], true);
  EXPECT_EQ(touch["settings"]["targetFps"], 120);
  const json &pad = after["profiles"][1];
  EXPECT_EQ(pad["settings"]["inputMode"], "pc");
  EXPECT_EQ(pad["settings"]["autoResolution"], false);
  EXPECT_EQ(pad["settings"]["width"], 2560);
  EXPECT_EQ(pad["settings"]["height"], 1440);
  EXPECT_EQ(pad["settings"]["refreshRate"], 144);
  EXPECT_EQ(pad["settings"]["fpsUnlock"], false);
  EXPECT_EQ(after["profiles"][2]["game"], "genshin");
  EXPECT_EQ(after["profiles"][0]["id"], "endfield-cn-1") << "ids, names and paths are the same";
  EXPECT_EQ(after["profiles"][0]["displayName"], "终末地触屏");
  EXPECT_EQ(sparxie::read_store(dir).state, "data");
}

TEST(SparxieGenerations, WithoutThePluginAnOldEndfieldProfileBlocksTheWriteInsteadOfBeingWiped) {
  const fs::path dir = fresh("sparxie-gen-repair-no-plugin");
  plugin_era_install(dir);
  const std::string before = R"({"schemaVersion":1,"profiles":[{"id":"e","displayName":"终末地","game":"endfield","executablePath":"C:/G/Endfield.exe","hoyo":{"endfieldInputMode":"touch"}}]})";
  write(dir / "config.json", before);
  sparxie::ProfileDraft genshin;
  genshin.game = "genshin";
  genshin.display_name = "原神";
  genshin.executable = "C:/G/YuanShen.exe";
  const auto refused = sparxie::add_profile(dir, genshin, {});
  EXPECT_FALSE(refused.status.ok);
  EXPECT_EQ(refused.state, "refused_unreadable");
  std::ifstream in(dir / "config.json");
  EXPECT_EQ(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()), before);
}

TEST(SparxieGenerations, EachGenerationReadsOnlyItsOwnSpellings) {
  const fs::path owners = fresh("sparxie-gen-owner-config");
  plugin_era_install(owners);
  endfield_plugin(owners, true);
  // the file the controller restored by hand on 2026-11-23: ext id, plugin settings
  const json doc = { { "schemaVersion", 1 }, { "selectedProfileId", "genshin-cn-1" }, { "profiles", json::array({
    { { "id", "genshin-cn-1" }, { "displayName", "原神触屏" }, { "game", "genshin" }, { "executablePath", "C:/G/YuanShen.exe" }, { "hoyo", { { "fpsUnlockEnabled", false }, { "targetFps", 120 }, { "genshinTouchUiScalePercent", 400 } } } },
    { { "id", "zzz-cn-1" }, { "displayName", "绝区零" }, { "game", "zenlessZoneZero" }, { "executablePath", "C:/G/ZenlessZoneZero.exe" } },
    { { "id", "endfield-cn-1" }, { "displayName", "终末地触屏" }, { "game", "ext.skyhua0224.endfield" }, { "executablePath", "C:/G/Endfield.exe" },
      { "settings", { { "inputMode", "touch" }, { "autoResolution", true }, { "width", 1920 }, { "height", 1080 }, { "refreshRate", 60 }, { "fpsUnlock", true }, { "targetFps", 120 } } } } }) } };
  write(owners / "config.json", doc.dump());
  const auto view = sparxie::read_store(owners);
  EXPECT_TRUE(view.status.ok) << view.status.error;
  EXPECT_EQ(view.state, "data");
  EXPECT_EQ(view.profile_count, 3);
  // the first generation cannot read an id that is not in its enum
  const fs::path old = fresh("sparxie-gen-owner-config-legacy");
  legacy_install(old, true);
  write(old / "config.json", doc.dump());
  EXPECT_EQ(sparxie::read_store(old).state, "unreadable");
  // validation rules of the second generation
  sparxie::SparxieBuild plugin;
  plugin.generation = sparxie::Generation::plugin;
  json bad = doc;
  bad["profiles"][2]["settings"] = json::array({ 1 });
  EXPECT_FALSE(sparxie::validate_document(bad, &plugin).ok) << "settings read as one JSON object: anything else makes the file unreadable";
  bad = doc;
  bad["profiles"][2]["game"] = "ext.Skyhua.endfield";
  EXPECT_FALSE(sparxie::validate_document(bad, &plugin).ok);
  bad = doc;
  bad["profiles"][2]["game"] = "ext.someone.else";
  bad["profiles"][2]["executablePath"] = "C:/Other/Whatever.exe";
  EXPECT_TRUE(sparxie::validate_document(bad, &plugin).ok) << "a plugin the host does not know is kept as it is";
  bad = doc;
  bad["profiles"][2]["game"] = "endfield";
  EXPECT_FALSE(sparxie::validate_document(bad, &plugin).ok);
  EXPECT_TRUE(sparxie::validate_document(bad).ok) << "judged as an unknown build both spellings pass";
}

TEST(SparxieGenerations, TheStatusSaysWhatEndfieldNeeds) {
  const fs::path root = fresh("sparxie-gen-status");
  sparxie::HostEnv env;
  env.host_data = root / "host";
  env.data_dir = root / "install" / "components" / "sparxie";
  env.apps_file = root / "apps.json";
  env.covers_dir = root / "covers";
  env.install_root = root / "install";
  env.component_dir = env.data_dir;
  env.process_running = [](const std::vector<std::string> &) { return false; };
  plugin_era_install(env.data_dir);
  auto status = sparxie::component_status(env);
  EXPECT_EQ(status.body["generation"], "plugin");
  EXPECT_EQ(status.body["endfield"]["support"], "plugin_missing");
  EXPECT_EQ(status.body["endfield"]["state"], "plugin_missing");
  EXPECT_EQ(status.body["endfield"]["message"], "需要终末地插件");
  EXPECT_EQ(status.body["endfield"]["plugin"]["installed"], false);
  endfield_plugin(env.data_dir, false);
  status = sparxie::component_status(env);
  EXPECT_EQ(status.body["endfield"]["support"], "plugin");
  EXPECT_EQ(status.body["endfield"]["state"], "plugin_untrusted");
  EXPECT_EQ(status.body["endfield"]["plugin"]["installed"], true);
  EXPECT_EQ(status.body["endfield"]["plugin"]["trusted"], false);
  endfield_plugin(env.data_dir, true);
  status = sparxie::component_status(env);
  EXPECT_EQ(status.body["endfield"]["state"], "ready");
  EXPECT_EQ(status.body["endfield"]["message"], "");
  EXPECT_EQ(status.body["endfield"]["plugin"]["trusted"], true);
  legacy_install(env.data_dir, true);
  write(env.data_dir / "version.ini", "exe_path=app-0.1.0-test\\Sparxie.Desktop.exe\n");
  status = sparxie::component_status(env);
  EXPECT_EQ(status.body["generation"], "legacy");
  EXPECT_EQ(status.body["endfield"]["support"], "builtin");
  EXPECT_EQ(status.body["endfield"]["state"], "ready");
}

namespace {
  // an Endfield install of the CN client and a Sparxie component beside the host
  sparxie::HostEnv
  endfield_env(const fs::path &root) {
    sparxie::HostEnv env;
    env.host_data = root / "host";
    env.apps_file = root / "apps.json";
    env.covers_dir = root / "covers";
    env.install_root = root / "install";
    env.component_dir = root / "install" / "components" / "sparxie";
    env.data_dir = env.component_dir;
    env.process_running = [](const std::vector<std::string> &) { return false; };
    write(root / "game" / "Endfield.exe", "x");
    return env;
  }
}  // namespace

TEST(SparxieGenerations, EndfieldKeyboardAndMouseGoesThroughThePluginWithoutATouchSwitch) {
  const fs::path root = fresh("sparxie-gen-host-kbm");
  sparxie::HostEnv env = endfield_env(root);
  plugin_era_install(env.component_dir);
  endfield_plugin(env.component_dir, true);
  ASSERT_FALSE(sparxie::component_status(env).body["touchUiSwitch"]) << "the second generation lists no capabilities";
  auto pad = sparxie::create_app(env, { { "path", (root / "game" / "Endfield.exe").string() }, { "region", "cn" }, { "mode", "touch" }, { "name", "终末地键鼠" }, { "options", { { "input", "kbm" }, { "fpsUnlock", true }, { "fps", 144 } } } });
  ASSERT_TRUE(pad.status.ok) << pad.status.error;
  EXPECT_EQ(pad.body["app"]["sparxie"]["mode"], "touch") << "the plugin's own input is the keyboard, mouse and gamepad; no direct start";
  EXPECT_NE(pad.body["app"]["cmd"].get<std::string>().find("--profile"), std::string::npos);
  const json doc = stored(env.data_dir);
  ASSERT_EQ(doc["profiles"].size(), 1u);
  EXPECT_EQ(doc["profiles"][0]["game"], "ext.skyhua0224.endfield");
  EXPECT_EQ(doc["profiles"][0]["settings"]["inputMode"], "pc");
  EXPECT_EQ(doc["profiles"][0]["settings"]["targetFps"], 144);
  EXPECT_EQ(sparxie::read_store(env.data_dir).state, "data");
}

TEST(SparxieGenerations, WithoutThePluginANewEndfieldTouchAppIsRefusedWithAClearCode) {
  const fs::path root = fresh("sparxie-gen-host-no-plugin");
  sparxie::HostEnv env = endfield_env(root);
  plugin_era_install(env.component_dir);
  auto refused = sparxie::create_app(env, { { "path", (root / "game" / "Endfield.exe").string() }, { "region", "cn" }, { "mode", "touch" }, { "name", "终末地" }, { "options", { { "input", "touch" } } } });
  EXPECT_FALSE(refused.status.ok);
  EXPECT_EQ(refused.body["error_code"], "endfield_plugin_missing");
  EXPECT_FALSE(fs::exists(env.data_dir / "config.json")) << "no profile was written";
  EXPECT_FALSE(fs::exists(env.apps_file)) << "and no application was made";
}

TEST(SparxieGenerations, AManifestWithTheWrongTypesIsNotInstalledAndNeverStopsTheHost) {
  const fs::path dir = fresh("sparxie-gen-bad-manifest");
  plugin_era_install(dir);
  for (const char *manifest : { R"({"id":123,"exeNames":["Endfield.exe"]})", R"({"id":null})", R"({"id":["ext.skyhua0224.endfield"]})", "[]", "42", "\"text\"" }) {
    endfield_plugin(dir, false, manifest);
    EXPECT_NO_THROW({
      const auto build = sparxie::detect_build(dir);
      EXPECT_FALSE(build.endfield.installed) << manifest;
    }) << manifest;
  }
  // wrong types in the members that are only read along the way: the plugin is still the plugin
  endfield_plugin(dir, true, R"({"id":"ext.skyhua0224.endfield","exeNames":["Endfield.exe"],"version":null,"settingsSchema":{"fields":[{"key":7},{"key":"inputMode"},"x"]}})");
  auto build = sparxie::detect_build(dir);
  EXPECT_TRUE(build.endfield.installed);
  EXPECT_EQ(build.endfield.version, "");
  EXPECT_EQ(build.endfield.keys, std::vector<std::string>({ "inputMode" }));
  EXPECT_TRUE(build.endfield.trusted);
  // a trust file whose records are of the wrong types: nothing is trusted, nothing throws
  write(dir / "plugin-trust.json", R"({"records":[{"id":1,"manifestSha256":2,"dllSha256":[]},"x",null]})");
  EXPECT_NO_THROW(build = sparxie::detect_build(dir));
  EXPECT_TRUE(build.endfield.installed);
  EXPECT_FALSE(build.endfield.trusted);
  // and the config of every game still reads and writes
  write(dir / kPluginDir / "plugin.json", R"({"id":123})");
  sparxie::ProfileDraft genshin;
  genshin.game = "genshin";
  genshin.display_name = "原神";
  genshin.executable = "C:/G/YuanShen.exe";
  EXPECT_TRUE(sparxie::add_profile(dir, genshin, {}).status.ok);
  EXPECT_EQ(sparxie::read_store(dir).state, "data");
  EXPECT_EQ(sparxie::add_profile(dir, endfield_draft(), {}).status.error_code, "endfield_plugin_missing");
}

TEST(SparxieGenerations, ASchemaThatDeclaresNoKeysGetsNoSettings) {
  const fs::path dir = fresh("sparxie-gen-empty-schema");
  plugin_era_install(dir);
  for (const char *manifest : { R"({"id":"ext.skyhua0224.endfield","exeNames":["Endfield.exe"],"settingsSchema":{"fields":[]}})", R"({"id":"ext.skyhua0224.endfield","exeNames":["Endfield.exe"]})" }) {
    endfield_plugin(dir, true, manifest);
    EXPECT_TRUE(sparxie::detect_build(dir).endfield.keys_known);
    fs::remove(dir / "config.json");
    ASSERT_TRUE(sparxie::add_profile(dir, endfield_draft(), {}).status.ok);
    const json doc = stored(dir);
    EXPECT_EQ(doc["profiles"][0]["game"], "ext.skyhua0224.endfield");
    EXPECT_TRUE(doc["profiles"][0]["settings"].is_object());
    EXPECT_TRUE(doc["profiles"][0]["settings"].empty()) << manifest << " " << doc["profiles"][0]["settings"].dump();
  }
  // a build described by hand that never read a schema takes every key the host manages
  sparxie::StoreHooks hooks;
  sparxie::SparxieBuild build;
  build.generation = sparxie::Generation::plugin;
  build.endfield.installed = true;
  hooks.build = build;
  const fs::path other = fresh("sparxie-gen-unread-schema");
  ASSERT_TRUE(sparxie::add_profile(other, endfield_draft(), hooks).status.ok);
  EXPECT_EQ(stored(other)["profiles"][0]["settings"].size(), 7u);
}

TEST(SparxieGenerations, RepairingAnOldEndfieldProfileKeepsTheSettingsTheHostDoesNotManage) {
  const fs::path dir = fresh("sparxie-gen-repair-keeps");
  plugin_era_install(dir);
  endfield_plugin(dir, true);
  const json doc = { { "schemaVersion", 1 }, { "selectedProfileId", "e" }, { "profiles", json::array({
    { { "id", "e" }, { "displayName", "终末地" }, { "game", "endfield" }, { "executablePath", "C:/G/Endfield.exe" },
      { "hoyo", { { "fpsUnlockEnabled", true }, { "targetFps", 120 }, { "endfieldInputMode", "pc" }, { "endfieldAutoResolution", true }, { "endfieldWidth", 1920 }, { "endfieldHeight", 1080 }, { "endfieldRefreshRate", 90 } } },
      { "settings", { { "uiScalePercent", 165 }, { "safeAreaMode", "manual" }, { "inputMode", "touch" } } } } }) } };
  write(dir / "config.json", doc.dump());
  sparxie::ProfileDraft genshin;
  genshin.game = "genshin";
  genshin.display_name = "原神";
  genshin.executable = "C:/G/YuanShen.exe";
  ASSERT_TRUE(sparxie::add_profile(dir, genshin, {}).status.ok);
  const json after = stored(dir);
  const json &settings = after["profiles"][0]["settings"];
  EXPECT_EQ(after["profiles"][0]["game"], "ext.skyhua0224.endfield");
  EXPECT_FALSE(after["profiles"][0].contains("hoyo"));
  EXPECT_EQ(settings["uiScalePercent"], 165) << "the person's own";
  EXPECT_EQ(settings["safeAreaMode"], "manual");
  EXPECT_EQ(settings["inputMode"], "pc") << "the host's keys come from the old profile's options";
  EXPECT_EQ(settings["refreshRate"], 90);
  EXPECT_EQ(settings["targetFps"], 120);
}
