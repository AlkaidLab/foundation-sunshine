/**
 * @file tests/unit/test_log_retention.cpp
 * @brief Keeping the previous run's log across a restart (src/log_retention.h).
 */
#include "src/log_retention.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

#include <gtest/gtest.h>

namespace {
  namespace fs = std::filesystem;

  class LogRetention: public ::testing::Test {
  protected:
    void
    SetUp() override {
      directory = fs::temp_directory_path() / ("log-retention-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
      fs::create_directories(directory);
      log = directory / "sunshine.log";
      archive = directory / "logs_archive";
    }

    void
    TearDown() override {
      std::error_code ignored;
      fs::remove_all(directory, ignored);
    }

    void
    write(const fs::path &path, const std::string &text) {
      std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
    }

    std::string
    read(const fs::path &path) {
      std::ifstream in(path, std::ios::binary);
      return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
    }

    // One host start: keep the previous log, then write a new one.
    log_retention::kept_e
    start_run(const std::string &text, int keep = 3) {
      std::error_code error;
      const auto kept = log_retention::retain_previous_run(log, archive, keep, error);
      write(log, text);
      return kept;
    }

    // A clean stop: the log sink moves the closed log into the archive folder as sunshine.log0000N.
    void
    stop_cleanly(int number) {
      fs::create_directories(archive);
      char name[32];
      std::snprintf(name, sizeof(name), "sunshine.log%05d", number);
      fs::rename(log, archive / name);
    }

    fs::path directory;
    fs::path log;
    fs::path archive;
  };
}  // namespace

TEST_F(LogRetention, TheLogOfTheCrashedRunSurvivesTheRestart) {
  start_run("run 1: stream started\nrun 1: about to crash\n");
  start_run("run 2: service restarted 7 s later\n");
  EXPECT_EQ(read(log), "run 2: service restarted 7 s later\n");
  EXPECT_EQ(read(log_retention::backup_path(log, 1)), "run 1: stream started\nrun 1: about to crash\n");
}

TEST_F(LogRetention, OlderRunsShiftUpAndTheOldestIsDropped) {
  for (int run = 1; run <= 5; ++run) {
    start_run("run " + std::to_string(run), 3);
  }
  EXPECT_EQ(read(log), "run 5");
  EXPECT_EQ(read(log_retention::backup_path(log, 1)), "run 4");
  EXPECT_EQ(read(log_retention::backup_path(log, 2)), "run 3");
  EXPECT_EQ(read(log_retention::backup_path(log, 3)), "run 2");
  EXPECT_FALSE(fs::exists(log_retention::backup_path(log, 4)));
}

TEST_F(LogRetention, FirstStartAndAnEmptyLogMakeNoBackup) {
  std::error_code error;
  EXPECT_EQ(log_retention::retain_previous_run(log, archive, 3, error), log_retention::kept_e::nothing);
  EXPECT_FALSE(fs::exists(log_retention::backup_path(log, 1)));
  write(log, "");
  EXPECT_EQ(log_retention::retain_previous_run(log, archive, 3, error), log_retention::kept_e::nothing);
  EXPECT_FALSE(fs::exists(log_retention::backup_path(log, 1)));
}

// The 2026-11-12 upgrade: the service stopped cleanly, the log sink moved sunshine.log into logs_archive, and the new process found no sunshine.log.
TEST_F(LogRetention, ACleanStopLeavesTheLogInTheArchiveAndItBecomesDotOne) {
  EXPECT_EQ(start_run("run 1"), log_retention::kept_e::nothing);
  stop_cleanly(4);
  EXPECT_FALSE(fs::exists(log));
  EXPECT_EQ(start_run("run 2"), log_retention::kept_e::from_archive);
  EXPECT_EQ(read(log_retention::backup_path(log, 1)), "run 1");
  EXPECT_EQ(read(archive / "sunshine.log00004"), "run 1") << "the archive keeps its own copy";
  EXPECT_EQ(read(log), "run 2");
}

TEST_F(LogRetention, TheNewestArchivedFileIsTheOneKept) {
  fs::create_directories(archive);
  // No gap between the writes on purpose: on Windows the ucrt stat path behind std::filesystem::last_write_time has whole seconds, so both
  // files carry the same write time and the counter in the name decides (2026-11-12: this tie picked sunshine.log00003 on the VM).
  write(archive / "sunshine.log00003", "old");
  write(archive / "sunshine.log00004", "newest");
  write(archive / "unrelated.txt", "not a log");
  std::error_code error;
  EXPECT_EQ(log_retention::retain_previous_run(log, archive, 3, error), log_retention::kept_e::from_archive);
  EXPECT_EQ(read(log_retention::backup_path(log, 1)), "newest");
}

TEST_F(LogRetention, ACrashAfterACleanRunKeepsBothInOrder) {
  start_run("run 1");
  stop_cleanly(1);
  start_run("run 2, crashes");  // .1 = run 1
  EXPECT_EQ(start_run("run 3"), log_retention::kept_e::leftover);
  EXPECT_EQ(read(log_retention::backup_path(log, 1)), "run 2, crashes");
  EXPECT_EQ(read(log_retention::backup_path(log, 2)), "run 1");
}

TEST_F(LogRetention, NothingIsDeletedBeyondTheBackups) {
  write(directory / "sunshine.log.keep-me", "unrelated");
  start_run("a");
  start_run("b");
  EXPECT_EQ(read(directory / "sunshine.log.keep-me"), "unrelated");
}
