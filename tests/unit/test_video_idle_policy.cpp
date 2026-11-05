/**
 * @file tests/unit/test_video_idle_policy.cpp
 * @brief The variable-refresh-rate encode loop must never leave a client without video.
 */
#include <gtest/gtest.h>

#include "src/video_idle_policy.h"

using namespace std::chrono_literals;

namespace {
  bool
  skip(bool vrr, bool new_frame, bool idr, int min_fps, bool boost, std::uint64_t encoded, std::chrono::milliseconds idle) {
    return video::vrr_may_skip_encode(vrr, new_frame, idr, min_fps, boost, encoded, idle);
  }
}  // namespace

TEST(VideoIdlePolicy, OffWhenVariableRefreshRateIsOff) {
  EXPECT_FALSE(skip(false, false, false, 0, false, 10, 0ms));
}

TEST(VideoIdlePolicy, EncodesEveryNewFrame) {
  EXPECT_FALSE(skip(true, true, false, 0, false, 10, 0ms));
}

TEST(VideoIdlePolicy, SkipsWhileTheSourceIsStaticAndRecent) {
  EXPECT_TRUE(skip(true, false, false, 0, false, 10, 16ms));
  EXPECT_TRUE(skip(true, false, false, 0, false, 10, 499ms));
}

TEST(VideoIdlePolicy, RepeatsAfterTheKeepaliveWithoutAnyChange) {
  EXPECT_FALSE(skip(true, false, false, 0, false, 10, 500ms));
  EXPECT_FALSE(skip(true, false, false, 0, false, 10, 5s));
}

TEST(VideoIdlePolicy, TheFirstFrameOfASessionIsNeverSkipped) {
  // a static desktop at the start used to send nothing at all: black screen, then the client gave up
  EXPECT_FALSE(skip(true, false, false, 0, false, 0, 0ms));
  EXPECT_FALSE(skip(true, false, false, 0, false, 0, 10ms));
}

TEST(VideoIdlePolicy, AKeyFrameRequestIsAnswered) {
  EXPECT_FALSE(skip(true, false, true, 0, false, 10, 0ms));
}

TEST(VideoIdlePolicy, AMinimumRateOrAnInputBoostKeepsItsOwnCadence) {
  EXPECT_FALSE(skip(true, false, false, 30, false, 10, 0ms));
  EXPECT_FALSE(skip(true, false, false, 0, true, 10, 0ms));
}
