/**
 * @file tests/unit/test_ds5_rumble_policy.cpp
 * @brief Which DualSense output reports move the motors, how a stop arrives, and the report log.
 */
#include "src/ds5/rumble_policy.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>

namespace {
  using ds5::rumble::decision_t;
  using ds5::rumble::reason_e;
  using ds5::rumble::report_t;
  using ds5::rumble::tracker_t;

  /* The rule of the host before this policy: only the two motor bits raise a rumble. */
  bool
  earlier_guard_raises(const report_t &report) {
    return (report.flag0 & 1u) != 0u || (report.flag2 & 4u) != 0u;
  }

  TEST(Ds5RumblePolicy, MotorBitsCarryTheValues) {
    tracker_t tracker;
    auto decision = tracker.decide({ 0x01, 0, 0, 90, 30 });
    EXPECT_TRUE(decision.raise);
    EXPECT_EQ(decision.left, 30);
    EXPECT_EQ(decision.right, 90);
    EXPECT_EQ(decision.reason, reason_e::motor_bits);
    decision = tracker.decide({ 0x00, 0, 0x04, 91, 31 });
    EXPECT_TRUE(decision.raise);
    EXPECT_EQ(decision.left, 31);
    EXPECT_EQ(decision.right, 91);
  }

  /* A report with haptics select alone and nonzero motor bytes has to move the motors: nothing proves
   * that it must not, and the earlier rule left such a game without rumble. */
  TEST(Ds5RumblePolicy, HapticsSelectWithValuesMovesTheMotors) {
    const report_t report { 0x02, 0, 0, 90, 30 };
    EXPECT_FALSE(earlier_guard_raises(report));
    tracker_t tracker;
    const auto decision = tracker.decide(report);
    EXPECT_TRUE(decision.raise);
    EXPECT_EQ(decision.left, 30);
    EXPECT_EQ(decision.right, 90);
    EXPECT_EQ(decision.reason, reason_e::haptics_select_values);
    EXPECT_TRUE(tracker.running());
  }

  TEST(Ds5RumblePolicy, HapticsSelectWithoutValuesStartsNothing) {
    tracker_t tracker;
    const auto decision = tracker.decide({ 0x02, 0, 0, 0, 0 });
    EXPECT_FALSE(decision.raise);
    EXPECT_FALSE(tracker.running());
  }

  TEST(Ds5RumblePolicy, EveryWayOfStoppingArrives) {
    {
      // Zero values under a motor bit.
      tracker_t tracker;
      ASSERT_TRUE(tracker.decide({ 0x01, 0, 0, 10, 10 }).raise);
      const auto decision = tracker.decide({ 0x01, 0, 0, 0, 0 });
      EXPECT_TRUE(decision.raise);
      EXPECT_EQ(decision.left | decision.right, 0);
      EXPECT_FALSE(tracker.running());
    }
    {
      // A select report with zero values ends a rumble that started with select alone.
      tracker_t tracker;
      ASSERT_TRUE(tracker.decide({ 0x02, 0, 0, 40, 40 }).raise);
      const auto decision = tracker.decide({ 0x02, 0, 0, 0, 0 });
      EXPECT_TRUE(decision.raise);
      EXPECT_EQ(decision.left | decision.right, 0);
      EXPECT_EQ(decision.reason, reason_e::haptics_select_stop);
      EXPECT_FALSE(tracker.decide({ 0x02, 0, 0, 0, 0 }).raise);
    }
    {
      // SDL ends a rumble with a report that has no enable bit.
      tracker_t tracker;
      ASSERT_TRUE(tracker.decide({ 0x03, 0, 0, 40, 40 }).raise);
      const auto decision = tracker.decide({ 0, 0, 0, 0, 0 });
      EXPECT_TRUE(decision.raise);
      EXPECT_EQ(decision.reason, reason_e::empty_report_stop);
      EXPECT_FALSE(tracker.decide({ 0, 0, 0, 0, 0 }).raise);
    }
    {
      // The pad goes away while the game still rumbles.
      tracker_t tracker;
      ASSERT_TRUE(tracker.decide({ 0x01, 0, 0, 40, 40 }).raise);
      EXPECT_TRUE(tracker.take_stop());
      EXPECT_FALSE(tracker.take_stop());
    }
  }

  TEST(Ds5RumblePolicy, OtherReportsDoNotTouchARunningRumble) {
    tracker_t tracker;
    ASSERT_TRUE(tracker.decide({ 0x01, 0, 0, 40, 40 }).raise);
    // A lightbar, trigger or player LED report carries no motor information.
    EXPECT_FALSE(tracker.decide({ 0x00, 0x04, 0, 0, 0 }).raise);
    EXPECT_FALSE(tracker.decide({ 0x0c, 0x00, 0, 0, 0 }).raise);
    EXPECT_FALSE(tracker.decide({ 0x00, 0x10, 0, 0, 0 }).raise);
    EXPECT_TRUE(tracker.running());
    // An empty report with nothing running changes nothing.
    tracker_t idle;
    EXPECT_FALSE(idle.decide({ 0, 0, 0, 0, 0 }).raise);
    EXPECT_FALSE(idle.take_stop());
  }

  TEST(Ds5RumbleLog, OneLinePerDistinctCombinationAndRateLimited) {
    using namespace std::chrono_literals;
    ds5::rumble::combination_log_t log(3, 100ms);
    const auto start = std::chrono::steady_clock::time_point {} + 1s;
    EXPECT_TRUE(log.should_log({ 0x02, 0, 0, 50, 20 }, start));
    // The same combination is not logged again, whatever the motor values are.
    EXPECT_FALSE(log.should_log({ 0x02, 0, 0, 51, 21 }, start + 1s));
    // Zero and nonzero motors are different combinations; the gap holds the next one back ...
    EXPECT_FALSE(log.should_log({ 0x02, 0, 0, 0, 0 }, start + 10ms));
    // ... and it is offered again with its next report.
    EXPECT_TRUE(log.should_log({ 0x02, 0, 0, 0, 0 }, start + 150ms));
    EXPECT_TRUE(log.should_log({ 0x03, 0x04, 0x04, 0, 0 }, start + 300ms));
    // At most three lines per pad.
    EXPECT_FALSE(log.should_log({ 0xff, 0xff, 0xff, 1, 1 }, start + 1s));
    EXPECT_EQ(log.count(), 3u);
  }
}  // namespace
