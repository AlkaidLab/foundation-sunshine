#include "src/video_deadline.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <optional>

namespace {
  using history_t = video::frame_deadline_history_t;
  using namespace std::chrono_literals;

  history_t::time_point_t at(std::chrono::milliseconds time) {
    return history_t::time_point_t {time};
  }
}

TEST(VideoDeadline, DelayedOutputsResolveTheirOwnSubmittedInput) {
  history_t history;
  history.bind(1, at(10ms), at(20ms));
  history.bind(2, at(30ms), at(40ms));
  // Output 1 arrives after input 2. It must retain 10, never latest/now 30/40.
  ASSERT_EQ(history.find(1), at(10ms));
  ASSERT_EQ(history.find(2), at(30ms));
  EXPECT_EQ(history.find(1), at(10ms));  // Multiple output packets for one input.
}

TEST(VideoDeadline, EvictedAndUnknownIdentityStayUnknownInsteadOfUsingLatest) {
  history_t history;
  EXPECT_FALSE(history.find(0));
  EXPECT_FALSE(history.find(UINT64_MAX));
  history.bind(1, at(10ms), at(20ms));
  history.bind(2, at(30ms), at(40ms));
  history.bind(1 + history_t::capacity, at(50ms), at(60ms));
  EXPECT_FALSE(history.find(1));
  EXPECT_FALSE(history.find(3));
  EXPECT_EQ(history.find(2), at(30ms));
  EXPECT_EQ(history.find(1 + history_t::capacity), at(50ms));
}

TEST(VideoDeadline, RepeatedFrameHasOwnOriginAndRetainsNullRtpTimestamp) {
  history_t history;
  const std::optional<history_t::time_point_t> duplicate_timestamp;
  history.bind(1, at(10ms), at(20ms));
  history.bind(2, duplicate_timestamp, at(40ms));
  history.bind(3, duplicate_timestamp, at(70ms));
  EXPECT_FALSE(duplicate_timestamp);
  EXPECT_EQ(history.find(2), at(40ms));
  EXPECT_EQ(history.find(3), at(70ms));
  EXPECT_EQ(history.find(1), at(10ms));
}

TEST(VideoDeadline, DefaultFallbackIsBoundedByActualSubmissionTime) {
  history_t history;
  const auto before = history_t::clock_t::now();
  history.bind(1, std::nullopt);
  const auto after = history_t::clock_t::now();
  ASSERT_TRUE(history.find(1));
  EXPECT_GE(*history.find(1), before);
  EXPECT_LE(*history.find(1), after);
}

TEST(VideoDeadline, NewEncoderHistoryCannotInheritPreviousSessionOrigin) {
  history_t old_session;
  old_session.bind(1, at(10ms), at(20ms));
  history_t new_session;
  EXPECT_FALSE(new_session.find(1));
  new_session.bind(2, std::nullopt, at(40ms));
  EXPECT_EQ(old_session.find(1), at(10ms));
  EXPECT_FALSE(old_session.find(2));
  EXPECT_EQ(new_session.find(2), at(40ms));
}

TEST(VideoDeadline, FullIdentityAndMovePreserveOriginsWithoutSentinelCollision) {
  history_t original;
  original.bind(UINT64_MAX, at(10ms), at(20ms));
  history_t moved = std::move(original);
  EXPECT_EQ(moved.find(UINT64_MAX), at(10ms));
  EXPECT_FALSE(moved.find(UINT64_MAX - 1));
  EXPECT_FALSE(moved.find(UINT64_MAX - history_t::capacity));
}
