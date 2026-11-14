#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include "src/sparxie/store.h"
namespace fs = std::filesystem;
TEST(Store, AppendPreservesUnknownFieldsAndBackups) {
  const fs::path dir = fs::temp_directory_path() / "sparxie-store-1";
  fs::remove_all(dir);
  fs::create_directories(dir);
  {
    std::ofstream seed(dir / "config.json");
    seed << R"({"schemaVersion":1,"selectedProfileId":"","desktop":{"theme":"dark"},"profiles":[{"id":"keep","displayName":"Old","game":"genshin","executablePath":"","future":1}]})";
  }
  sparxie::ProfileDraft draft;
  draft.display_name = "Genshin";
  draft.game = "genshin";
  draft.executable = (dir / "YuanShen.exe").string();
  draft.options = sparxie::json{{"fpsUnlock", false}, {"fps", 90}};
  sparxie::StoreHooks hooks;
  hooks.utc_stamp = [] { return std::string("20261110T120000Z"); };
  auto wrote = sparxie::add_profile(dir, draft, hooks);
  EXPECT_TRUE(wrote.status.ok) << wrote.status.error;
  EXPECT_EQ(wrote.state, "written");
  auto view = sparxie::read_store(dir);
  EXPECT_EQ(view.state, "data");
  EXPECT_EQ(view.document["desktop"]["theme"], "dark");
  EXPECT_EQ(view.document["profiles"][0]["future"], 1);
  EXPECT_EQ(view.document["profiles"][1]["hoyo"]["fpsUnlockEnabled"], false);
  EXPECT_EQ(view.document["profiles"][1]["hoyo"]["targetFps"], 90);
  EXPECT_EQ(view.document["profiles"][1]["game"], "genshin");
  EXPECT_TRUE(fs::exists(dir / wrote.backup));
  for (int n = 0; n < 6; ++n) {
    hooks.utc_stamp = [n] { return std::string("20261110T12000") + std::to_string(n) + "Z"; };
    sparxie::ProfileDraft extra = draft;
    extra.display_name = "N" + std::to_string(n);
    extra.id.clear();
    auto again = sparxie::add_profile(dir, extra, hooks);
    EXPECT_TRUE(again.status.ok) << again.status.error;
  }
  int backups = 0;
  for (const auto &entry : fs::directory_iterator(dir))
    if (entry.path().filename().string().rfind("config.backup-", 0) == 0) ++backups;
  EXPECT_LE(backups, 5);
}
TEST(Store, RefusesSchemaRunningAndRestoresCorruptWrite) {
  const fs::path dir = fs::temp_directory_path() / "sparxie-store-2";
  fs::remove_all(dir);
  fs::create_directories(dir);
  const std::string original = R"({"schemaVersion":1,"selectedProfileId":"keep","desktop":{"keep":true},"profiles":[{"id":"keep","displayName":"Old","game":"genshin","executablePath":""}]})";
  {
    std::ofstream out(dir / "config.json");
    out << original;
  }
  sparxie::ProfileDraft draft;
  draft.id = "genshin-1";
  draft.display_name = "Genshin";
  draft.game = "genshin";
  draft.executable = "C:/Games/YuanShen.exe";
  sparxie::StoreHooks running;
  running.running = true;
  auto busy = sparxie::add_profile(dir, draft, running);
  EXPECT_EQ(busy.status.error_code, "sparxie_running");
  {
    std::ofstream out(dir / "config.json");
    out << R"({"schemaVersion":2,"profiles":[]})";
  }
  auto schema = sparxie::add_profile(dir, draft, {});
  EXPECT_EQ(schema.status.error_code, "schema_unsupported");
  {
    std::ofstream out(dir / "config.json");
    out << original;
  }
  sparxie::StoreHooks hooks;
  hooks.utc_stamp = [] { return std::string("20261110T130000Z"); };
  hooks.after_replace = [](const fs::path &path) {
    std::ofstream out(path, std::ios::trunc);
    out << "not-json";
  };
  auto restored = sparxie::add_profile(dir, draft, hooks);
  EXPECT_EQ(restored.status.error_code, "write_failed");
  std::ifstream in(dir / "config.json");
  std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_NE(body.find("keep"), std::string::npos);
  EXPECT_EQ(body.find("not-json"), std::string::npos);
}
TEST(Store, CreatesWhenMissingAndUpdatesWithoutDroppingFields) {
  const fs::path dir = fs::temp_directory_path() / "sparxie-store-3";
  fs::remove_all(dir);
  sparxie::ProfileDraft draft;
  draft.id = "zzz-1";
  draft.display_name = "ZZZ";
  draft.game = "zzz";
  draft.executable = "C:/Games/ZenlessZoneZero.exe";
  auto created = sparxie::add_profile(dir, draft, {});
  EXPECT_TRUE(created.status.ok) << created.status.error;
  EXPECT_EQ(created.state, "created");
  auto view = sparxie::read_store(dir);
  EXPECT_FALSE(view.document["profiles"][0].contains("hoyo"));
  view.document["profiles"][0]["vendorExtra"] = 7;
  {
    std::ofstream out(dir / "config.json");
    out << view.document.dump(2);
  }
  sparxie::ProfileDraft update;
  update.display_name = "ZZZ renamed";
  update.game = "zzz";
  update.executable = draft.executable;
  auto updated = sparxie::update_profile(dir, "zzz-1", update, {});
  EXPECT_TRUE(updated.status.ok) << updated.status.error;
  auto after = sparxie::read_store(dir);
  EXPECT_EQ(after.document["profiles"][0]["displayName"], "ZZZ renamed");
  EXPECT_EQ(after.document["profiles"][0]["vendorExtra"], 7);
  EXPECT_FALSE(after.document["profiles"][0].contains("hoyo"));
}
// keyboard, mouse and gamepad is a Sparxie session without the touch interface: the profile says so (HoyoSettings.touch_ui_enabled)
TEST(Store, TheInputChoiceIsWrittenAsTouchUiEnabledAndReadBack) {
  const fs::path dir = fs::temp_directory_path() / "sparxie-store-touchui";
  fs::remove_all(dir);
  fs::create_directories(dir);
  sparxie::StoreHooks hooks;
  hooks.utc_stamp = [] { return std::string("20261111T120000Z"); };
  sparxie::ProfileDraft draft;
  draft.display_name = "Star Rail pad";
  draft.game = "starrail";
  draft.executable = (dir / "StarRail.exe").string();
  draft.options = sparxie::json { { "input", "kbm" }, { "fpsUnlock", true }, { "fps", 144 } };
  auto kbm = sparxie::add_profile(dir, draft, hooks);
  ASSERT_TRUE(kbm.status.ok) << kbm.status.error;
  draft.display_name = "Star Rail touch";
  draft.options = sparxie::json { { "input", "touch" }, { "fpsUnlock", true }, { "fps", 144 } };
  auto touch = sparxie::add_profile(dir, draft, hooks);
  ASSERT_TRUE(touch.status.ok) << touch.status.error;
  auto view = sparxie::read_store(dir);
  ASSERT_TRUE(view.status.ok) << view.status.error;
  EXPECT_EQ(view.document["profiles"][0]["hoyo"]["touchUiEnabled"], false);
  EXPECT_EQ(view.document["profiles"][0]["hoyo"]["fpsUnlockEnabled"], true);
  EXPECT_EQ(view.document["profiles"][1]["hoyo"]["touchUiEnabled"], true);
  // the editor changes the input of an existing profile: the same profile now says the other thing
  draft.display_name = "Star Rail pad";
  draft.options = sparxie::json { { "input", "kbm" }, { "fpsUnlock", true }, { "fps", 144 } };
  auto again = sparxie::update_profile(dir, touch.profile, draft, hooks);
  ASSERT_TRUE(again.status.ok) << again.status.error;
  view = sparxie::read_store(dir);
  EXPECT_EQ(view.document["profiles"][1]["hoyo"]["touchUiEnabled"], false);
  // and the stored value is the input of an imported application; a profile from an older Sparxie (no field) is a touch one
  EXPECT_EQ(sparxie::options_from_hoyo(view.document["profiles"][1]["hoyo"])["input"], "kbm");
  EXPECT_EQ(sparxie::options_from_hoyo(sparxie::json { { "fpsUnlockEnabled", true } })["input"], "touch");
  // a file whose value is not a boolean is refused like any other bad field
  sparxie::json bad = view.document;
  bad["profiles"][0]["hoyo"]["touchUiEnabled"] = "no";
  EXPECT_FALSE(sparxie::validate_document(bad).ok);
  // Zenless Zone Zero has no hoyo block, so there is nothing to say
  sparxie::ProfileDraft zzz;
  zzz.display_name = "ZZZ";
  zzz.game = "zzz";
  zzz.executable = (dir / "ZenlessZoneZero.exe").string();
  zzz.options = sparxie::json { { "input", "kbm" } };
  auto written = sparxie::add_profile(dir, zzz, hooks);
  ASSERT_TRUE(written.status.ok) << written.status.error;
  view = sparxie::read_store(dir);
  EXPECT_FALSE(view.document["profiles"][2].contains("hoyo"));
}
