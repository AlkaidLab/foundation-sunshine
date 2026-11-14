/**
 * @file tests/unit/test_build_info.cpp
 * @brief Build identity: the compiled record, the package decision and the JSON contract.
 */
#include "src/build_info.h"

#include <gtest/gtest.h>

#include <fstream>
#include <regex>

namespace {
  struct temp_dir_t {
    std::filesystem::path path;

    temp_dir_t() {
      path = std::filesystem::temp_directory_path() / ("build-info-test-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "-" + ::testing::UnitTest::GetInstance()->current_test_info()->name());
      std::filesystem::remove_all(path);
      std::filesystem::create_directories(path);
    }

    ~temp_dir_t() {
      std::error_code ec;
      std::filesystem::remove_all(path, ec);
    }

    void
    touch(const std::string &name) const {
      std::ofstream {path / name} << "x";
    }
  };
}  // namespace

TEST(BuildInfo, CompiledRecordHasTheConfiguredFields) {
  const auto info = build_info::compiled();
  EXPECT_FALSE(info.version.empty());
  EXPECT_TRUE(std::regex_match(info.commit, std::regex {"^([0-9a-f]{40})?$"})) << info.commit;
  EXPECT_TRUE(std::regex_match(info.channel, std::regex {"^(stable|beta|dev|local)$"})) << info.channel;
  EXPECT_TRUE(std::regex_match(info.build_time_utc, std::regex {R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$)"})) << info.build_time_utc;
  EXPECT_TRUE(std::regex_match(info.package, std::regex {"^(installer|portable|auto)$"})) << info.package;
  EXPECT_TRUE(std::regex_match(info.builder, std::regex {"^(ci|local)$"})) << info.builder;
  EXPECT_FALSE(info.variant.empty());
}

TEST(BuildInfo, AutoPackageFollowsTheInnoUninstaller) {
  EXPECT_EQ(build_info::resolve_package("auto", true), "installer");
  EXPECT_EQ(build_info::resolve_package("auto", false), "portable");
  // a build that fixed its package keeps it
  EXPECT_EQ(build_info::resolve_package("portable", true), "portable");
  EXPECT_EQ(build_info::resolve_package("installer", false), "installer");
}

TEST(BuildInfo, InnoUninstallerDetection) {
  temp_dir_t dir;
  EXPECT_FALSE(build_info::has_inno_uninstaller(dir.path));
  dir.touch("unins000.dat");
  dir.touch("uninstall.exe");
  dir.touch("unins0000.exe");
  EXPECT_FALSE(build_info::has_inno_uninstaller(dir.path));
  dir.touch("UNINS001.EXE");
  EXPECT_TRUE(build_info::has_inno_uninstaller(dir.path));
  EXPECT_FALSE(build_info::has_inno_uninstaller(dir.path / "missing"));
}

TEST(BuildInfo, CurrentResolvesAutoFromTheLayout) {
  temp_dir_t dir;
  auto portable = build_info::current(dir.path);
  dir.touch("unins000.exe");
  auto installer = build_info::current(dir.path);
  if (build_info::compiled().package == "auto") {
    EXPECT_EQ(portable.package, "portable");
    EXPECT_EQ(installer.package, "installer");
    EXPECT_EQ(installer.package_source, "layout");
  } else {
    EXPECT_EQ(portable.package, build_info::compiled().package);
    EXPECT_EQ(installer.package_source, "build");
  }
}

TEST(BuildInfo, JsonContract) {
  build_info::info_t info;
  info.version = "2026.1104.121813";
  info.commit = "96a1e2b6c0ffee00112233445566778899aabbcc";
  info.branch = "master";
  info.dirty = true;
  info.channel = "beta";
  info.build_time_utc = "2026-11-04T04:18:13Z";
  info.variant = "foundation-sunshine";
  info.package = "installer";
  info.package_source = "layout";
  info.builder = "ci";
  info.ci_run = "https://github.com/AlkaidLab/foundation-sunshine/actions/runs/1";
  const auto json = build_info::to_json(info);
  EXPECT_EQ(json["json_version"], build_info::JSON_VERSION);
  EXPECT_EQ(json["commit"], info.commit);
  EXPECT_EQ(json["commit_short"], "96a1e2b6c0ff");
  EXPECT_EQ(json["dirty"], true);
  EXPECT_EQ(json["channel"], "beta");
  EXPECT_EQ(json["build_time_utc"], "2026-11-04T04:18:13Z");
  EXPECT_EQ(json["variant"], "foundation-sunshine");
  EXPECT_EQ(json["package"], "installer");
  EXPECT_EQ(json["package_source"], "layout");
  EXPECT_EQ(json["builder"], "ci");
  for (const auto *key : {"version", "branch", "ci_run"}) {
    EXPECT_TRUE(json.contains(key)) << key;
  }
}

TEST(BuildInfo, ShortCommitOfShortInput) {
  EXPECT_EQ(build_info::short_commit(""), "");
  EXPECT_EQ(build_info::short_commit("abc"), "abc");
}

TEST(BuildInfo, ProcessStartIsAnIsoUtcTimeThatDoesNotChangeWhileTheProcessRuns) {
  const auto first = build_info::process_started_at_utc();
  EXPECT_TRUE(std::regex_match(first, std::regex(R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z)"))) << first;
  EXPECT_EQ(first, build_info::process_started_at_utc());
}
