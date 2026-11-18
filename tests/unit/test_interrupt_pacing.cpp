/**
 * @file tests/unit/test_interrupt_pacing.cpp
 * @brief Interrupt IN reply timing of the virtual devices, on a manual clock.
 */
#include <gtest/gtest.h>

#include <chrono>

#include "src/platform/interrupt_pacing.h"

namespace {
  using namespace std::chrono_literals;
  using pacing_t = platf::interrupt_pacing<std::chrono::steady_clock>;
  using tp = std::chrono::steady_clock::time_point;

  const tp t0 = tp {} + 1000s;
}  // namespace

TEST(InterruptPacing, FirstUrbIsAnsweredAtOnce) {
  pacing_t pacing;
  EXPECT_EQ(pacing.schedule(t0), t0);
}

TEST(InterruptPacing, IdleKeepsTheFourMillisecondHeartbeat) {
  pacing_t pacing;
  auto due = pacing.schedule(t0);
  pacing.sent(due);
  const auto next = pacing.schedule(due + 100us);
  EXPECT_EQ(next, due + 4ms);
  pacing.sent(next);
  EXPECT_EQ(pacing.schedule(next + 100us), next + 4ms);
}

TEST(InterruptPacing, NewDataWhileHeldMovesTheReplyUp) {
  pacing_t pacing;
  const auto first = pacing.schedule(t0);
  pacing.sent(first);
  const auto held = pacing.schedule(first + 100us);
  ASSERT_EQ(held, first + 4ms);
  tp moved;
  ASSERT_TRUE(pacing.new_data(first + 1500us, true, held, moved));
  EXPECT_EQ(moved, first + 1500us);
  EXPECT_TRUE(pacing.dirty());
}

TEST(InterruptPacing, NewDataNeverBeatsTheOneMillisecondFloor) {
  pacing_t pacing;
  const auto first = pacing.schedule(t0);
  pacing.sent(first);
  const auto held = pacing.schedule(first + 100us);
  tp moved;
  ASSERT_TRUE(pacing.new_data(first + 200us, true, held, moved));
  EXPECT_EQ(moved, first + 1ms);
}

TEST(InterruptPacing, DataThatArrivesLateDoesNotMoveAnAlreadyDueReply) {
  pacing_t pacing;
  const auto first = pacing.schedule(t0);
  pacing.sent(first);
  const auto held = pacing.schedule(first + 100us);
  tp moved;
  EXPECT_FALSE(pacing.new_data(held, true, held, moved));
}

TEST(InterruptPacing, DataBeforeTheNextUrbAnswersItAtOnceAfterTheFloor) {
  pacing_t pacing;
  const auto first = pacing.schedule(t0);
  pacing.sent(first);
  tp moved;
  EXPECT_FALSE(pacing.new_data(first + 300us, false, tp {}, moved));
  EXPECT_EQ(pacing.schedule(first + 400us), first + 1ms);
  EXPECT_EQ(pacing.schedule(first + 5ms) >= first + 5ms, true);
}

TEST(InterruptPacing, SendingClearsTheDirtyFlag) {
  pacing_t pacing;
  const auto first = pacing.schedule(t0);
  tp moved;
  pacing.new_data(first, false, tp {}, moved);
  EXPECT_TRUE(pacing.dirty());
  pacing.sent(first + 1ms);
  EXPECT_FALSE(pacing.dirty());
  EXPECT_EQ(pacing.schedule(first + 1ms + 50us), first + 5ms);
}

TEST(InterruptPacing, RepliesNeverLeaveCloserThanTheFloorEvenWhenSentLate) {
  pacing_t pacing;
  const auto first = pacing.schedule(t0);
  pacing.sent(first + 3ms);  // the timer fired late
  tp moved;
  pacing.new_data(first + 3ms, false, tp {}, moved);
  EXPECT_GE(pacing.schedule(first + 3ms + 10us), first + 4ms);
}

TEST(InterruptPacing, ResetForgetsTheClock) {
  pacing_t pacing;
  pacing.sent(t0);
  pacing.reset();
  EXPECT_EQ(pacing.schedule(t0 + 1ms), t0 + 1ms);
  EXPECT_FALSE(pacing.dirty());
}

TEST(InterruptPacing, ConfiguredPeriodsApply) {
  pacing_t pacing { 2ms, 10ms };
  const auto first = pacing.schedule(t0);
  pacing.sent(first);
  EXPECT_EQ(pacing.schedule(first), first + 10ms);
}
