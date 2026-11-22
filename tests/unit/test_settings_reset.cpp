/**
 * @file tests/unit/test_settings_reset.cpp
 * @brief Restoring the host settings to their defaults: the copy, the kept keys, the rotation, the restore and the one-at-a-time gate (src/settings_reset.h).
 */
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

#include "src/settings_reset.h"

namespace fs = std::filesystem;
using namespace settings_reset;

namespace {
  // the settings format for the tests: one `key = value` per line
  std::map<std::string, std::string>
  parse_lines(std::string_view text) {
    std::map<std::string, std::string> out;
    std::istringstream in { std::string { text } };
    for (std::string line; std::getline(in, line);) {
      const auto at = line.find(" = ");
      if (at == std::string::npos) continue;
      out.emplace(line.substr(0, at), line.substr(at + 3));
    }
    return out;
  }

  std::string
  read(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
  }

  void
  write(const fs::path &path, const std::string &text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
  }

  class SettingsReset: public testing::Test {
  protected:
    fs::path dir;
    paths_t paths;
    std::int64_t clock_ms = 0;
    parse_t parse = parse_lines;
    now_t now = [this] { return clock_ms += 1000; };
    static constexpr std::string_view conf =
      "adapter_name = GPU 0\n"
      "clients = [{\"uuid\":\"a\"}]\n"
      "hevc_mode = 2\n"
      "locale = zh\n"
      "min_bitrate = 4000\n"
      "port = 57989\n"
      "sunshine_name = Desk\n"
      "telemetry_consent = disabled\n"
      "widget_token = secret-token\n";

    void
    SetUp() override {
      dir = fs::temp_directory_path() / ("settings-reset-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
      fs::create_directories(dir);
      paths = { dir / "sunshine.conf", dir / "apps.json", dir / "bundled-apps.json" };
      clock_ms = parse_stamp("20261119-100000");
      write(paths.config, std::string { conf });
      write(paths.apps, R"({"apps":[{"name":"My Game"}]})");
      write(paths.bundled_apps, R"({"apps":[{"name":"Desktop"}]})");
      write(dir / "sunshine_state.json", R"({"root":{"named_devices":[{"name":"phone"}]}})");
    }
    void
    TearDown() override {
      std::error_code ignored;
      fs::remove_all(dir, ignored);
    }
    int
    copies() {
      return static_cast<int>(list_backups(paths).size());
    }
    int
    files(const std::string &prefix) {
      int n = 0;
      for (const auto &entry : fs::directory_iterator(dir))
        if (entry.path().filename().string().rfind(prefix, 0) == 0) ++n;
      return n;
    }
  };
}  // namespace

TEST_F(SettingsReset, TheStampIsTheUtcTimeAndReadsBack) {
  EXPECT_EQ(format_stamp(0), "19700101-000000");
  EXPECT_EQ(format_stamp(1795082400000), "20261119-100000");
  EXPECT_EQ(parse_stamp("20261119-100000"), 1795082400000);
  EXPECT_EQ(parse_stamp("20261119-100000-4"), 1795082400000);
  EXPECT_EQ(format_stamp(parse_stamp("20240229-235959")), "20240229-235959");
  EXPECT_EQ(parse_stamp("not-a-stamp"), 0);
  EXPECT_EQ(parse_stamp("20261319-100000"), 0);
}

TEST_F(SettingsReset, ResetCopiesTheSettingsThenKeepsOnlyTheKeptKeys) {
  const auto result = reset_to_defaults(paths, false, parse, now);
  ASSERT_TRUE(result);
  EXPECT_FALSE(result.apps_changed);
  ASSERT_FALSE(result.backup.empty());
  EXPECT_EQ(read(paths.config.string() + ".reset-" + result.backup), conf);

  const auto after = parse_lines(read(paths.config));
  const std::map<std::string, std::string> expected {
    { "clients", "[{\"uuid\":\"a\"}]" },
    { "locale", "zh" },
    { "port", "57989" },
    { "telemetry_consent", "disabled" },
    { "widget_token", "secret-token" },
  };
  EXPECT_EQ(after, expected);
}

TEST_F(SettingsReset, PairedDevicesAndTheApplicationListStayByDefault) {
  const auto state_before = read(dir / "sunshine_state.json");
  const auto apps_before = read(paths.apps);
  ASSERT_TRUE(reset_to_defaults(paths, false, parse, now));
  EXPECT_EQ(read(dir / "sunshine_state.json"), state_before);
  EXPECT_EQ(read(paths.apps), apps_before);
  EXPECT_EQ(files("apps.json.reset-"), 0);
}

TEST_F(SettingsReset, ClearingTheApplicationListCopiesItAndPutsTheBundledOneInPlace) {
  const auto apps_before = read(paths.apps);
  const auto result = reset_to_defaults(paths, true, parse, now);
  ASSERT_TRUE(result);
  EXPECT_TRUE(result.apps_changed);
  EXPECT_EQ(read(paths.apps), read(paths.bundled_apps));
  EXPECT_EQ(read(paths.apps.string() + ".reset-" + result.backup), apps_before);
  EXPECT_EQ(read(dir / "sunshine_state.json"), R"({"root":{"named_devices":[{"name":"phone"}]}})");
  EXPECT_TRUE(list_backups(paths).front().has_apps);
}

TEST_F(SettingsReset, WithoutTheBundledListNothingIsChanged) {
  fs::remove(paths.bundled_apps);
  const auto result = reset_to_defaults(paths, true, parse, now);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error, error_e::defaults_missing);
  EXPECT_EQ(read(paths.config), conf);
  EXPECT_EQ(copies(), 0);
  // the settings alone do not need it
  EXPECT_TRUE(reset_to_defaults(paths, false, parse, now));
}

TEST_F(SettingsReset, WhenTheApplicationListCannotBeWrittenTheSettingsGoBack) {
  paths.apps = dir / "missing-folder" / "apps.json";  // there is no list to copy and none can be written there
  const auto result = reset_to_defaults(paths, true, parse, now);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error, error_e::write_failed);
  EXPECT_EQ(read(paths.config), conf);
  EXPECT_EQ(copies(), 0);
  EXPECT_EQ(files("sunshine.conf.tmp"), 0);
}

TEST_F(SettingsReset, OnlyTheFiveNewestCopiesStayAndTheirApplicationListCopiesGoWithThem) {
  std::vector<std::string> made;
  for (int i = 0; i < 7; ++i) {
    write(paths.config, "sunshine_name = run" + std::to_string(i) + "\n");
    const auto result = reset_to_defaults(paths, true, parse, now);
    ASSERT_TRUE(result);
    made.push_back(result.backup);
  }
  const auto kept = list_backups(paths);
  ASSERT_EQ(kept.size(), kept_copies);
  EXPECT_EQ(kept.front().stamp, made.back());
  EXPECT_EQ(kept.back().stamp, made[2]);
  EXPECT_EQ(files("sunshine.conf.reset-"), 5);
  EXPECT_EQ(files("apps.json.reset-"), 5);
  EXPECT_FALSE(fs::exists(paths.config.string() + ".reset-" + made[0]));
  EXPECT_FALSE(fs::exists(paths.apps.string() + ".reset-" + made[1]));
}

TEST_F(SettingsReset, RestoreBringsTheSettingsBackAndCanBeTakenBack) {
  ASSERT_TRUE(reset_to_defaults(paths, false, parse, now));
  ASSERT_EQ(parse_lines(read(paths.config)).count("sunshine_name"), 0u);

  const auto restored = restore_latest(paths, parse, now);
  ASSERT_TRUE(restored);
  auto after = parse_lines(read(paths.config));
  EXPECT_EQ(after.at("sunshine_name"), "Desk");
  EXPECT_EQ(after.at("hevc_mode"), "2");
  EXPECT_EQ(after.at("widget_token"), "secret-token");
  EXPECT_FALSE(restored.apps_changed);
  EXPECT_EQ(copies(), 2);  // the one before the reset and the defaults that were there when the restore ran

  // the restore itself is a copy: restoring again returns to the defaults
  ASSERT_TRUE(restore_latest(paths, parse, now));
  after = parse_lines(read(paths.config));
  EXPECT_EQ(after.count("sunshine_name"), 0u);
  EXPECT_EQ(after.at("locale"), "zh");
}

TEST_F(SettingsReset, RestoreKeepsTheKeptKeysAsTheyAreNow) {
  ASSERT_TRUE(reset_to_defaults(paths, false, parse, now));
  write(paths.config, "locale = en\nport = 47989\nsunshine_name = Other\n");
  ASSERT_TRUE(restore_latest(paths, parse, now));
  const auto after = parse_lines(read(paths.config));
  EXPECT_EQ(after.at("locale"), "en");
  EXPECT_EQ(after.at("port"), "47989");
  EXPECT_EQ(after.at("sunshine_name"), "Desk");
  // a kept key that is not there now is not there after, even when the copy has it
  EXPECT_EQ(after.count("widget_token"), 0u);
  EXPECT_EQ(after.count("clients"), 0u);
}

TEST_F(SettingsReset, RestoreAlsoPutsBackAnApplicationListThatWasCleared) {
  const auto apps_before = read(paths.apps);
  ASSERT_TRUE(reset_to_defaults(paths, true, parse, now));
  ASSERT_EQ(read(paths.apps), read(paths.bundled_apps));
  const auto restored = restore_latest(paths, parse, now);
  ASSERT_TRUE(restored);
  EXPECT_TRUE(restored.apps_changed);
  EXPECT_EQ(read(paths.apps), apps_before);
  // and what the list was before the restore is copied too
  EXPECT_EQ(read(paths.apps.string() + ".reset-" + restored.backup), read(paths.bundled_apps));
}

TEST_F(SettingsReset, RestoreWithoutACopyDoesNothing) {
  const auto result = restore_latest(paths, parse, now);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error, error_e::no_backup);
  EXPECT_EQ(read(paths.config), conf);
}

TEST_F(SettingsReset, TwoCopiesInTheSameSecondKeepTheirOrder) {
  now_t frozen = [this] { return clock_ms; };
  ASSERT_TRUE(reset_to_defaults(paths, false, parse, frozen));
  write(paths.config, "sunshine_name = second\n");
  ASSERT_TRUE(reset_to_defaults(paths, false, parse, frozen));
  const auto both = list_backups(paths);
  ASSERT_EQ(both.size(), 2u);
  EXPECT_NE(both[0].stamp, both[1].stamp);
  EXPECT_EQ(both[0].created_ms, both[1].created_ms);
  ASSERT_TRUE(restore_latest(paths, parse, frozen));
  EXPECT_EQ(parse_lines(read(paths.config)).at("sunshine_name"), "second");
}

TEST_F(SettingsReset, WithoutASettingsFileThereIsNothingToCopy) {
  fs::remove(paths.config);
  const auto result = reset_to_defaults(paths, false, parse, now);
  ASSERT_TRUE(result);
  EXPECT_TRUE(result.backup.empty());
  EXPECT_EQ(copies(), 0);
  EXPECT_TRUE(fs::exists(paths.config));
}

TEST_F(SettingsReset, ACopyThatIsNotAFileIsNotListedAndNothingIsRestored) {
  ASSERT_TRUE(reset_to_defaults(paths, false, parse, now));
  const auto latest = list_backups(paths).front();
  // the copy is gone (a folder is not a copy): there is nothing to restore
  fs::remove(paths.config.string() + ".reset-" + latest.stamp);
  EXPECT_EQ(restore_latest(paths, parse, now).error, error_e::no_backup);
}

TEST_F(SettingsReset, AnApplicationListCopyThatCannotBeReadStopsTheRestoreBeforeAnyChange) {
  const auto apps_before = read(paths.apps);
  ASSERT_TRUE(reset_to_defaults(paths, true, parse, now));
  const auto latest = list_backups(paths).front();
  ASSERT_TRUE(latest.has_apps);
  // the copy of the list became a folder: it cannot be read, and must not be taken for an empty list
  const auto apps_copy = paths.apps.string() + ".reset-" + latest.stamp;
  fs::remove(apps_copy);
  fs::create_directories(apps_copy);
  const auto config_before = read(paths.config);
  const auto apps_now = read(paths.apps);
  const auto result = restore_latest(paths, parse, now);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error, error_e::read_failed);
  EXPECT_EQ(read(paths.config), config_before);
  EXPECT_EQ(read(paths.apps), apps_now);
  EXPECT_NE(apps_now, apps_before);
  EXPECT_EQ(copies(), 1);
}

TEST_F(SettingsReset, TwoClicksAtOnceMakeOneCopy) {
  gate_t gate;
  std::atomic<int> worked { 0 }, busy { 0 };
  std::vector<std::thread> clicks;
  std::atomic<bool> go { false };
  for (int i = 0; i < 8; ++i) {
    clicks.emplace_back([&] {
      while (!go.load()) std::this_thread::yield();
      if (!gate.try_enter()) {
        ++busy;
        return;
      }
      const auto result = reset_to_defaults(paths, false, parse, [] { return std::int64_t { 1795082400000 }; });
      gate.leave(static_cast<bool>(result));
      if (result) ++worked;
    });
  }
  go = true;
  for (auto &click : clicks) click.join();
  EXPECT_EQ(worked.load(), 1);
  EXPECT_EQ(busy.load(), 7);
  EXPECT_EQ(copies(), 1);
}

TEST_F(SettingsReset, TheGateStaysClosedAfterAResetAndOpensAgainAfterAFailure) {
  gate_t gate;
  ASSERT_TRUE(gate.try_enter());
  EXPECT_FALSE(gate.try_enter());  // one is running
  gate.leave(false);  // it failed: the next click may try
  ASSERT_TRUE(gate.try_enter());
  gate.leave(true);  // it worked: the host restarts, no more clicks count
  EXPECT_FALSE(gate.try_enter());
}

TEST_F(SettingsReset, EveryKeptKeyIsListedOnce) {
  std::set<std::string_view> unique { kept_keys().begin(), kept_keys().end() };
  EXPECT_EQ(unique.size(), kept_keys().size());
  for (const auto key : { "widget_token", "clients", "locale", "port", "telemetry_consent", "telemetry_errors", "file_state", "file_apps", "credentials_file" }) {
    EXPECT_TRUE(unique.contains(key)) << key;
  }
}
