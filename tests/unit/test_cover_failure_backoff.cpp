/**
 * @file tests/unit/test_cover_failure_backoff.cpp
 * @brief Tests for the failed cover download memory.
 */
#include "src/cover_failure_backoff.h"

#include <gtest/gtest.h>

using namespace std::chrono_literals;

TEST(CoverFailureBackoff, UnknownUrlIsNotBlocked) {
  http::cover_failure_backoff_t backoff;
  EXPECT_FALSE(backoff.blocked("https://cdn.example/a.jpg"));
}

TEST(CoverFailureBackoff, FailureBlocksForTheFirstWaitThenReleases) {
  http::cover_failure_backoff_t backoff { 10min, 6h };
  const auto t0 = http::cover_failure_backoff_t::clock::now();
  backoff.note_failure("u", t0);
  EXPECT_TRUE(backoff.blocked("u", t0 + 9min));
  EXPECT_FALSE(backoff.blocked("u", t0 + 10min + 1s));
  EXPECT_FALSE(backoff.blocked("other", t0));
}

TEST(CoverFailureBackoff, EveryFailureDoublesTheWaitUpToTheLimit) {
  http::cover_failure_backoff_t backoff { 10min, 1h };
  auto now = http::cover_failure_backoff_t::clock::now();
  backoff.note_failure("u", now);  // 10 min
  now += 11min;
  backoff.note_failure("u", now);  // 20 min
  EXPECT_TRUE(backoff.blocked("u", now + 19min));
  EXPECT_FALSE(backoff.blocked("u", now + 21min));
  now += 21min;
  backoff.note_failure("u", now);  // 40 min
  now += 41min;
  backoff.note_failure("u", now);  // 80 min, held to 1 h
  EXPECT_TRUE(backoff.blocked("u", now + 59min));
  EXPECT_FALSE(backoff.blocked("u", now + 61min));
}

TEST(CoverFailureBackoff, SuccessForgetsTheUrl) {
  http::cover_failure_backoff_t backoff;
  const auto t0 = http::cover_failure_backoff_t::clock::now();
  backoff.note_failure("u", t0);
  backoff.note_success("u");
  EXPECT_FALSE(backoff.blocked("u", t0));
}

TEST(CoverFailureBackoff, WhenFullTheWaitsThatRanOutMakeRoomAndTheOthersStay) {
  http::cover_failure_backoff_t backoff { 10min, 6h, 4 };
  const auto t0 = http::cover_failure_backoff_t::clock::now();
  backoff.note_failure("old0", t0);
  backoff.note_failure("old1", t0);
  backoff.note_failure("late0", t0 + 20min);
  backoff.note_failure("late1", t0 + 20min);
  // the first two have run out by now: a new URL takes the place of the expired ones, the waiting ones are untouched
  backoff.note_failure("new", t0 + 25min);
  EXPECT_TRUE(backoff.blocked("new", t0 + 26min));
  EXPECT_TRUE(backoff.blocked("late0", t0 + 26min));
  EXPECT_TRUE(backoff.blocked("late1", t0 + 26min));
}

TEST(CoverFailureBackoff, WhenFullOfRunningWaitsANewUrlIsNotRemembered) {
  http::cover_failure_backoff_t backoff { 10min, 6h, 4 };
  const auto t0 = http::cover_failure_backoff_t::clock::now();
  for (int i = 0; i < 4; ++i) backoff.note_failure("u" + std::to_string(i), t0);
  backoff.note_failure("extra", t0 + 1min);
  EXPECT_FALSE(backoff.blocked("extra", t0 + 2min));
  for (int i = 0; i < 4; ++i) EXPECT_TRUE(backoff.blocked("u" + std::to_string(i), t0 + 2min));
}
