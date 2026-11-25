#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <vector>
#include "src/sparxie/store.h"
namespace fs = std::filesystem;

namespace {
  std::string
  contents(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }

  std::set<std::string>
  backup_names(const fs::path &dir) {
    std::set<std::string> names;
    for (const auto &entry : fs::directory_iterator(dir)) {
      const auto name = entry.path().filename().string();
      if (name.starts_with("config.backup-")) names.insert(name);
    }
    return names;
  }
}  // namespace

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
  // If the clock moved back, the rollback copy can have the oldest name.
  for (int n = 0; n < 5; ++n)
    std::ofstream(dir / ("config.backup-20261111T12000" + std::to_string(n) + "Z.json")) << original;
  auto restored = sparxie::add_profile(dir, draft, hooks);
  EXPECT_EQ(restored.status.error_code, "write_failed");
  EXPECT_EQ(contents(dir / "config.json"), original);
  EXPECT_EQ(contents(dir / restored.backup), original);
  hooks.after_replace = {};
  hooks.fail_replace_times = 3;
  const auto failed = sparxie::add_profile(dir, draft, hooks);
  EXPECT_EQ(failed.status.error_code, "write_failed");
  EXPECT_EQ(contents(dir / "config.json"), original);
  hooks.fail_replace_times = 0;
  const auto success = sparxie::add_profile(dir, draft, hooks);
  ASSERT_TRUE(success.status.ok) << success.status.error;
  EXPECT_EQ(contents(dir / success.backup), original);
  EXPECT_EQ(backup_names(dir).size(), 5u);
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

TEST(Store, RepeatedIdenticalUpdatesPreserveTheFileAndRealEditsBackItUp) {
  const fs::path dir = fs::temp_directory_path() / "sparxie-store-unchanged";
  fs::remove_all(dir);
  fs::create_directories(dir);
  const fs::path path = dir / "config.json";
  const std::string original = R"({"profiles":[{"id":"zzz-1","displayName":"ZZZ","game":"zenlessZoneZero","executablePath":"C:/Games/ZenlessZoneZero.exe","launch":{"extraArguments":"-use-d3d12"},"future":1}],"selectedProfileId":"zzz-1","schemaVersion":1})";
  std::ofstream(path, std::ios::binary) << original;
  fs::last_write_time(path, fs::file_time_type::clock::now() - std::chrono::hours(1));
  const auto original_time = fs::last_write_time(path);
  sparxie::ProfileDraft draft;
  draft.display_name = "ZZZ";
  draft.game = "zzz";
  draft.executable = "C:/Games/ZenlessZoneZero.exe";
  draft.options = { { "fpsUnlock", true }, { "fps", 120 } };
  sparxie::StoreHooks hooks;
  hooks.utc_stamp = [] { return std::string("20261110T120000Z"); };

  for (int n = 0; n < 10; ++n) {
    const auto same = sparxie::update_profile(dir, "zzz-1", draft, hooks);
    ASSERT_TRUE(same.status.ok) << same.status.error;
    if (n == 0) EXPECT_EQ(same.state, "unchanged");
    EXPECT_TRUE(same.backup.empty());
  }
  EXPECT_EQ(contents(path), original);
  EXPECT_EQ(fs::last_write_time(path), original_time);
  EXPECT_TRUE(backup_names(dir).empty());

  draft.display_name = "ZZZ renamed";
  const auto changed = sparxie::update_profile(dir, "zzz-1", draft, hooks);
  ASSERT_TRUE(changed.status.ok) << changed.status.error;
  EXPECT_EQ(changed.state, "written");
  EXPECT_EQ(backup_names(dir), (std::set<std::string> { changed.backup }));
  EXPECT_EQ(contents(dir / changed.backup), original);
  const auto after = sparxie::read_store(dir);
  ASSERT_TRUE(after.status.ok) << after.status.error;
  const auto &profile = after.document["profiles"][0];
  EXPECT_EQ(profile["displayName"], "ZZZ renamed");
  EXPECT_EQ(profile["launch"]["extraArguments"], "-use-d3d12");
  EXPECT_EQ(profile["future"], 1);
}

TEST(Store, SameSecondBackupsKeepTheFiveMostRecentEdits) {
  const fs::path dir = fs::temp_directory_path() / "sparxie-store-numbered";
  fs::remove_all(dir);
  sparxie::ProfileDraft draft;
  draft.id = "zzz-1";
  draft.display_name = "Revision 0";
  draft.game = "zzz";
  draft.executable = "C:/Games/ZenlessZoneZero.exe";
  ASSERT_TRUE(sparxie::add_profile(dir, draft, {}).status.ok);
  sparxie::StoreHooks hooks;
  hooks.utc_stamp = [] { return std::string("20261110T120000Z"); };
  for (int n = 1; n <= 12; ++n) {
    draft.display_name = "Revision " + std::to_string(n);
    const auto wrote = sparxie::update_profile(dir, draft.id, draft, hooks);
    ASSERT_TRUE(wrote.status.ok) << wrote.status.error;
  }
  std::set<std::string> expected;
  for (int n = 8; n <= 12; ++n)
    expected.insert("config.backup-20261110T120000Z-" + std::to_string(n) + ".json");
  EXPECT_EQ(backup_names(dir), expected);
  const auto last = sparxie::json::parse(contents(dir / "config.backup-20261110T120000Z-12.json"), nullptr, false);
  ASSERT_TRUE(last.is_object());
  EXPECT_EQ(last["profiles"][0]["displayName"], "Revision 11");

  const auto legacy = dir / "config.backup-20261110T120000Z-x.json";
  std::ofstream(legacy) << last.dump();
  draft.display_name = "Revision 13";
  const auto with_legacy = sparxie::update_profile(dir, draft.id, draft, hooks);
  ASSERT_TRUE(with_legacy.status.ok) << with_legacy.status.error;
  EXPECT_EQ(with_legacy.backup, "config.backup-20261110T120000Z-101.json");
  EXPECT_TRUE(fs::is_regular_file(legacy));
  EXPECT_EQ(backup_names(dir).size(), 5u);
}

TEST(Store, AnUnchangedProfileUpdateStillSavesRepairsToTheDocument) {
  const fs::path dir = fs::temp_directory_path() / "sparxie-store-repair-unchanged";
  fs::remove_all(dir);
  fs::create_directories(dir);
  const std::string original = R"({"schemaVersion":1,"profiles":[{"id":"zzz-1","displayName":"ZZZ","game":"zenlessZoneZero","executablePath":"C:/Games/ZenlessZoneZero.exe"},{"id":"endfield-1","displayName":"Endfield","game":"endfield","hoyo":{"endfieldWidth":0,"endfieldHeight":0}}]})";
  std::ofstream(dir / "config.json", std::ios::binary) << original;
  sparxie::ProfileDraft draft;
  draft.display_name = "ZZZ";
  draft.game = "zzz";
  draft.executable = "C:/Games/ZenlessZoneZero.exe";

  const auto repaired = sparxie::update_profile(dir, "zzz-1", draft, {});
  ASSERT_TRUE(repaired.status.ok) << repaired.status.error;
  EXPECT_EQ(repaired.state, "written");
  EXPECT_EQ(contents(dir / repaired.backup), original);
  const auto view = sparxie::read_store(dir);
  ASSERT_TRUE(view.status.ok) << view.status.error;
  EXPECT_EQ(view.document["profiles"][1]["hoyo"]["endfieldWidth"], 1920);
  EXPECT_EQ(view.document["profiles"][1]["hoyo"]["endfieldHeight"], 1080);
  EXPECT_EQ(sparxie::update_profile(dir, "zzz-1", draft, {}).state, "unchanged");
  EXPECT_EQ(backup_names(dir).size(), 1u);
}

TEST(Store, RotationWarnsAboutLockedOrInaccessibleBackupsAndRetries) {
  const fs::path dir = fs::temp_directory_path() / "sparxie-store-retry-prune";
  fs::remove_all(dir);
  sparxie::ProfileDraft draft;
  draft.id = "zzz-1";
  draft.display_name = "ZZZ";
  draft.game = "zzz";
  draft.executable = "C:/Games/ZenlessZoneZero.exe";
  ASSERT_TRUE(sparxie::add_profile(dir, draft, {}).status.ok);
  const std::string current = contents(dir / "config.json");
  std::vector<fs::path> copies;
  for (int n = 0; n < 8; ++n) {
    copies.push_back(dir / ("config.backup-20261110T12000" + std::to_string(n) + "Z.json"));
    std::ofstream(copies.back(), std::ios::binary) << current;
  }
  const auto backup_dir = dir / "config.backup-20261109T000000Z.json";
  fs::create_directory(backup_dir);
  const std::vector<std::string> unrelated { "config.invalid-20261109T000000Z.json", "config.repaired-20261109T000000Z.json", "config.backup-manual.json", "config.backup-20261109T000000Z-manual.json" };
  for (const auto &name : unrelated)
    std::ofstream(dir / name) << "keep";
  std::error_code link_error;
  const auto link = dir / "config.backup-20261109T000001Z.json";
  fs::create_symlink(dir / unrelated.front(), link, link_error);

  std::vector<std::string> warnings;
  sparxie::StoreHooks hooks;
  hooks.warn = [&warnings](const auto &message) { warnings.push_back(message); };
  hooks.remove_backup = [&](const fs::path &path, std::error_code &ec) {
    if (path == copies[0])
      ec = std::make_error_code(std::errc::device_or_resource_busy);
    else if (path == copies[1])
      ec = std::make_error_code(std::errc::permission_denied);
    else
      fs::remove(path, ec);
  };
  const auto unchanged = sparxie::update_profile(dir, draft.id, draft, hooks);
  ASSERT_TRUE(unchanged.status.ok) << unchanged.status.error;
  EXPECT_EQ(unchanged.state, "unchanged");
  ASSERT_EQ(warnings.size(), 2u);
  const std::string diagnostics = warnings[0] + warnings[1];
  EXPECT_NE(diagnostics.find(copies[0].filename().string()), std::string::npos);
  EXPECT_NE(diagnostics.find(copies[1].filename().string()), std::string::npos);
  EXPECT_NE(diagnostics.find(std::make_error_code(std::errc::permission_denied).message()), std::string::npos);
  EXPECT_NE(diagnostics.find(std::make_error_code(std::errc::device_or_resource_busy).message()), std::string::npos);
  EXPECT_TRUE(fs::exists(copies[0]));
  EXPECT_TRUE(fs::exists(copies[1]));
  EXPECT_FALSE(fs::exists(copies[2]));

  hooks.remove_backup = {};
  EXPECT_EQ(sparxie::update_profile(dir, draft.id, draft, hooks).state, "unchanged");
  for (std::size_t i = 0; i < copies.size(); ++i)
    EXPECT_EQ(fs::exists(copies[i]), i >= 3) << copies[i];
  EXPECT_EQ(contents(dir / "config.json"), current);
  EXPECT_TRUE(fs::is_directory(backup_dir));
  if (!link_error) EXPECT_TRUE(fs::is_symlink(fs::symlink_status(link)));
  for (const auto &name : unrelated)
    EXPECT_EQ(contents(dir / name), "keep") << name;
}

#if !defined(_WIN32)
TEST(Store, AnUnreadableBackupListingOnlyWarnsOnAnUnchangedUpdate) {
  const fs::path dir = fs::temp_directory_path() / "sparxie-store-listing-error";
  fs::remove_all(dir);
  sparxie::ProfileDraft draft;
  draft.id = "zzz-1";
  draft.display_name = "ZZZ";
  draft.game = "zzz";
  draft.executable = "C:/Games/ZenlessZoneZero.exe";
  ASSERT_TRUE(sparxie::add_profile(dir, draft, {}).status.ok);
  const std::string original = contents(dir / "config.json");
  const auto permissions = fs::status(dir).permissions();
  fs::permissions(dir, fs::perms::owner_exec);
  std::error_code denied;
  const fs::directory_iterator probe(dir, denied);
  if (!denied) {
    fs::permissions(dir, permissions);
    GTEST_SKIP() << "The test process can enumerate the directory despite its permissions";
  }
  std::vector<std::string> warnings;
  sparxie::StoreHooks hooks;
  hooks.warn = [&warnings](const auto &message) { warnings.push_back(message); };
  const auto unchanged = sparxie::update_profile(dir, draft.id, draft, hooks);
  fs::permissions(dir, permissions);

  ASSERT_TRUE(unchanged.status.ok) << unchanged.status.error;
  EXPECT_EQ(unchanged.state, "unchanged");
  ASSERT_EQ(warnings.size(), 1u);
  EXPECT_NE(warnings[0].find("Could not list config backups"), std::string::npos);
  EXPECT_NE(warnings[0].find(denied.message()), std::string::npos);
  EXPECT_EQ(contents(dir / "config.json"), original);
}
#endif
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
