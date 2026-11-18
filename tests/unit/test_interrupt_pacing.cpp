/**
 * @file tests/unit/test_interrupt_pacing.cpp
 * @brief Interrupt IN reply timing of the virtual devices, on a manual clock.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <vector>

#include "src/platform/interrupt_pacing.h"

namespace {
  using namespace std::chrono_literals;
  using clock_type = std::chrono::steady_clock;
  using tp = clock_type::time_point;
  using lane_t = platf::interrupt_lane<int, clock_type>;

  const tp t0 = tp {} + 1000s;

  /**
   * A host that keeps @p outstanding URBs queued and posts a new one 60 us after each reply,
   * with a game that changes a report at @p event_hz between @p events_from and @p events_to.
   */
  struct host_t {
    lane_t lane;
    std::uint32_t next_sequence { 1 };
    std::vector<tp> replies;
    std::vector<tp> resubmits;

    explicit host_t(int outstanding) {
      for (int i = 0; i < outstanding; ++i) lane.hold(next_sequence++, 0);
    }

    void
    run(tp from, tp to, int event_hz, tp events_from, tp events_to) {
      auto next_event = events_from;
      for (auto now = from; now < to; now += 10us) {
        if (event_hz > 0 && now >= events_from && now < events_to && now >= next_event) {
          next_event += std::chrono::microseconds(1000000 / event_hz);
          lane.new_data();
        }
        while (!resubmits.empty() && resubmits.front() <= now) {
          resubmits.erase(resubmits.begin());
          lane.hold(next_sequence++, 0);
        }
        if (lane.take_if_due(now)) {
          replies.push_back(now);
          resubmits.push_back(now + 60us);
        }
      }
    }

    std::chrono::microseconds
    longest_silence(tp from, tp to) const {
      std::chrono::microseconds longest { 0 };
      tp previous = from;
      for (const auto at : replies) {
        if (at < from) continue;
        if (at > to) break;
        longest = std::max(longest, std::chrono::duration_cast<std::chrono::microseconds>(at - previous));
        previous = at;
      }
      return std::max(longest, std::chrono::duration_cast<std::chrono::microseconds>(to - previous));
    }

    std::size_t
    count(tp from, tp to) const {
      return static_cast<std::size_t>(std::count_if(replies.begin(), replies.end(), [&](tp at) { return at >= from && at < to; }));
    }
  };
}  // namespace

TEST(InterruptLane, FirstUrbIsAnsweredAtOnce) {
  lane_t lane;
  lane.hold(1, 7);
  const auto item = lane.take_if_due(t0);
  ASSERT_TRUE(item.has_value());
  EXPECT_EQ(*item, 7);
}

TEST(InterruptLane, NothingHeldMeansNothingDue) {
  lane_t lane;
  EXPECT_FALSE(lane.next_due().has_value());
  EXPECT_FALSE(lane.take_if_due(t0).has_value());
}

TEST(InterruptLane, IdleKeepsTheFourMillisecondHeartbeat) {
  lane_t lane;
  lane.hold(1, 1);
  ASSERT_TRUE(lane.take_if_due(t0));
  lane.hold(2, 2);
  EXPECT_FALSE(lane.take_if_due(t0 + 3999us));
  EXPECT_TRUE(lane.take_if_due(t0 + 4ms));
  lane.hold(3, 3);
  EXPECT_EQ(lane.next_due(), t0 + 8ms);
}

TEST(InterruptLane, NewDataAnswersAHeldUrbAfterTheOneMillisecondFloor) {
  lane_t lane;
  lane.hold(1, 1);
  ASSERT_TRUE(lane.take_if_due(t0));
  lane.hold(2, 2);
  lane.new_data();
  EXPECT_EQ(lane.next_due(), t0 + 1ms);
  EXPECT_FALSE(lane.take_if_due(t0 + 999us));
  EXPECT_TRUE(lane.take_if_due(t0 + 1ms));
}

TEST(InterruptLane, DataBeforeTheNextUrbAnswersItAtOnceAfterTheFloor) {
  lane_t lane;
  lane.hold(1, 1);
  ASSERT_TRUE(lane.take_if_due(t0));
  lane.new_data();
  lane.hold(2, 2);
  EXPECT_EQ(lane.next_due(), t0 + 1ms);
}

TEST(InterruptLane, SendingClearsTheDirtyFlag) {
  lane_t lane;
  lane.hold(1, 1);
  lane.new_data();
  ASSERT_TRUE(lane.take_if_due(t0));
  EXPECT_FALSE(lane.dirty());
  lane.hold(2, 2);
  EXPECT_EQ(lane.next_due(), t0 + 4ms);
}

TEST(InterruptLane, ALateTimerNeverBringsTwoRepliesCloserThanTheFloor) {
  lane_t lane;
  lane.hold(1, 1);
  ASSERT_TRUE(lane.take_if_due(t0 + 3ms));  // the timer fired late
  lane.hold(2, 2);
  lane.new_data();
  EXPECT_EQ(lane.next_due(), t0 + 4ms);
}

TEST(InterruptLane, RepliesHeldTogetherLeaveOneIntervalApart) {
  lane_t lane;
  for (std::uint32_t i = 1; i <= 3; ++i) lane.hold(i, static_cast<int>(i));
  EXPECT_EQ(lane.take_if_due(t0), 1);
  EXPECT_FALSE(lane.take_if_due(t0 + 1ms));
  EXPECT_EQ(lane.take_if_due(t0 + 4ms), 2);
  EXPECT_EQ(lane.take_if_due(t0 + 8ms), 3);
}

TEST(InterruptLane, CancelRemovesOnlyAHeldReply) {
  lane_t lane;
  lane.hold(1, 1);
  lane.hold(2, 2);
  EXPECT_TRUE(lane.cancel(2));
  EXPECT_FALSE(lane.cancel(2));
  EXPECT_EQ(lane.held(), 1u);
  ASSERT_TRUE(lane.take_if_due(t0));
  EXPECT_FALSE(lane.cancel(1));
}

TEST(InterruptLane, ResetForgetsTheClock) {
  lane_t lane;
  lane.hold(1, 1);
  lane.new_data();
  ASSERT_TRUE(lane.take_if_due(t0));
  lane.hold(2, 2);
  lane.reset();
  EXPECT_EQ(lane.held(), 0u);
  EXPECT_FALSE(lane.dirty());
  lane.hold(3, 3);
  EXPECT_TRUE(lane.take_if_due(t0 + 1us));
}

TEST(InterruptLane, ConfiguredPeriodsApply) {
  lane_t lane { 2ms, 10ms };
  lane.hold(1, 1);
  ASSERT_TRUE(lane.take_if_due(t0));
  lane.hold(2, 2);
  EXPECT_EQ(lane.next_due(), t0 + 10ms);
  lane.new_data();
  EXPECT_EQ(lane.next_due(), t0 + 2ms);
}

/* The heartbeat must go on between sparse events: a game that presses buttons at 125 Hz still
 * has the 4 ms heartbeat underneath, whatever the number of URBs the host keeps outstanding. */
TEST(InterruptLane, HeartbeatContinuesBetweenSparseEvents) {
  for (const int outstanding : { 1, 2, 4, 8, 32 }) {
    host_t host { outstanding };
    host.run(t0, t0 + 6s, 125, t0, t0 + 6s);
    EXPECT_LE(host.longest_silence(t0, t0 + 6s), 4100us) << "outstanding " << outstanding;
    /* 125 events a second plus the heartbeat that falls between them: no fewer than 250 a second. */
    EXPECT_GE(host.count(t0 + 1s, t0 + 6s), 5u * 250u - 5u) << "outstanding " << outstanding;
  }
}

/* A second of 500 Hz events, then quiet: the heartbeat resumes within one period. */
TEST(InterruptLane, HeartbeatResumesWithinFourMillisecondsAfterABurst) {
  for (const int outstanding : { 1, 2, 4, 8, 32 }) {
    host_t host { outstanding };
    host.run(t0, t0 + 4s, 500, t0 + 500ms, t0 + 1500ms);
    EXPECT_LE(host.longest_silence(t0 + 1500ms, t0 + 4s), 4100us) << "outstanding " << outstanding;
    EXPECT_LE(host.longest_silence(t0, t0 + 4s), 4100us) << "outstanding " << outstanding;
  }
}

TEST(InterruptLane, EventsAnswerAtTheirOwnRateAndNeverCloserThanTheFloor) {
  host_t host { 2 };
  host.run(t0, t0 + 2s, 500, t0, t0 + 2s);
  for (std::size_t i = 1; i < host.replies.size(); ++i) {
    EXPECT_GE(host.replies[i] - host.replies[i - 1], 1ms) << "reply " << i;
  }
  EXPECT_NEAR(static_cast<double>(host.count(t0 + 500ms, t0 + 1500ms)), 500.0, 5.0);
}

TEST(InterruptLane, IdleReplyRateIsTheHeartbeat) {
  host_t host { 3 };
  host.run(t0, t0 + 3s, 0, t0, t0);
  EXPECT_NEAR(static_cast<double>(host.count(t0 + 1s, t0 + 2s)), 250.0, 2.0);
}
