/**
 * @file tests/unit/test_encoder_probe_guard.cpp
 * @brief Tests for the encoder probe marker.
 */
#include "src/encoder_probe_guard.h"

#include <chrono>
#include <gtest/gtest.h>

namespace {
  std::filesystem::path
  fresh_dir(const char *name) {
    auto dir = std::filesystem::temp_directory_path() / (std::string { "alk-probe-guard-" } + name + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    return dir;
  }
}  // namespace

TEST(EncoderProbeGuard, NoMarkerMeansNoInterruptedProbe) {
  const auto dir = fresh_dir("none");
  EXPECT_EQ(encoder_probe_guard::take_interrupted(dir), "");
  std::filesystem::remove_all(dir);
}

TEST(EncoderProbeGuard, ALeftBehindMarkerNamesTheEncoderOnceAndIsRemoved) {
  const auto dir = fresh_dir("left");
  encoder_probe_guard::enter(dir, "nvenc");  // the process ended here: no leave()
  EXPECT_EQ(encoder_probe_guard::take_interrupted(dir), "nvenc");
  EXPECT_EQ(encoder_probe_guard::take_interrupted(dir), "");
  std::filesystem::remove_all(dir);
}

TEST(EncoderProbeGuard, AProbeThatEndsLeavesNothing) {
  const auto dir = fresh_dir("ended");
  {
    encoder_probe_guard::scope_t scope { dir, "amdvce" };
    EXPECT_TRUE(std::filesystem::exists(encoder_probe_guard::marker_path(dir)));
  }
  EXPECT_FALSE(std::filesystem::exists(encoder_probe_guard::marker_path(dir)));
  EXPECT_EQ(encoder_probe_guard::take_interrupted(dir), "");
  std::filesystem::remove_all(dir);
}

TEST(EncoderProbeGuard, AMissingDirectoryIsCreatedAndAnOddMarkerIsIgnored) {
  const auto dir = fresh_dir("odd") / "sub";
  encoder_probe_guard::enter(dir, "quicksync");
  EXPECT_EQ(encoder_probe_guard::take_interrupted(dir), "quicksync");
  encoder_probe_guard::enter(dir, std::string(200, 'x'));
  EXPECT_EQ(encoder_probe_guard::take_interrupted(dir), "");
  std::filesystem::remove_all(dir.parent_path());
}

TEST(EncoderProbeGuard, SkipsOnlyTheInterruptedEncoderUnlessAskedFor) {
  using encoder_probe_guard::should_skip;
  EXPECT_TRUE(should_skip("nvenc", "nvenc", "", false));
  EXPECT_FALSE(should_skip("nvenc", "quicksync", "", false));
  EXPECT_FALSE(should_skip("", "nvenc", "", false));
  // set in the settings, or a check the person started: tried anyway
  EXPECT_FALSE(should_skip("nvenc", "nvenc", "nvenc", false));
  EXPECT_FALSE(should_skip("nvenc", "nvenc", "", true));
  // another encoder set in the settings does not protect this one
  EXPECT_TRUE(should_skip("nvenc", "nvenc", "quicksync", false));
}

TEST(EncoderProbeGuard, TheMarkerIsReadBeforeTheNextProbeWritesItsOwn) {
  const auto dir = fresh_dir("order");
  encoder_probe_guard::enter(dir, "amdvce");  // left by the previous process
  const auto interrupted = encoder_probe_guard::take_interrupted(dir);
  {
    // a probe of an encoder that is not skipped (set in the settings): its own marker no longer hides the old one
    encoder_probe_guard::scope_t scope { dir, "amdvce" };
    EXPECT_EQ(interrupted, "amdvce");
  }
  EXPECT_EQ(encoder_probe_guard::take_interrupted(dir), "");
  std::filesystem::remove_all(dir);
}
