/**
 * @file tests/unit/test_interrupt_pacing.cpp
 * @brief Interrupt IN reply timing of the virtual devices, on a manual clock.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <deque>
#include <vector>

#include "src/platform/interrupt_pacing.h"

namespace {
  using namespace std::chrono_literals;
  using clock_type = std::chrono::steady_clock;
  using tp = clock_type::time_point;
  using lane_t = platf::interrupt_lane<int, clock_type>;
  using platf::report_change;

  const tp t0 = tp {} + 1000s;

  struct reply_t {
    tp at;
    bool edge;
    int edge_number;  ///< 1 for the reply to the first button change, 2 for the second, 0 for any other reply
  };

  /**
   * A host that keeps @p outstanding URBs queued and posts a new one 60 us after each reply,
   * with a game that changes the report at the times it is given.
   */
  struct host_t {
    lane_t lane;
    unsigned pads { 1 };
    std::uint32_t next_sequence { 1 };
    std::vector<reply_t> replies;
    std::vector<tp> resubmits;
    int edge_replies { 0 };

    explicit host_t(int outstanding, unsigned active_pads = 1):
        pads(active_pads) {
      for (int i = 0; i < outstanding; ++i) lane.hold(next_sequence++, 0);
    }

    void
    run(tp from, tp to, const std::vector<std::pair<tp, report_change>> &events) {
      auto next_event = events.begin();
      for (auto now = from; now < to; now += 10us) {
        while (next_event != events.end() && next_event->first <= now) lane.new_data((next_event++)->second);
        while (!resubmits.empty() && resubmits.front() <= now) {
          resubmits.erase(resubmits.begin());
          lane.hold(next_sequence++, 0);
        }
        if (const auto taken = lane.take_if_due(now, pads)) {
          replies.push_back({ now, taken->edge, taken->edge ? ++edge_replies : 0 });
          resubmits.push_back(now + 60us);
        }
      }
    }

    std::chrono::microseconds
    longest_silence(tp from, tp to) const {
      std::chrono::microseconds longest { 0 };
      tp previous = from;
      for (const auto &reply : replies) {
        if (reply.at < from) continue;
        if (reply.at > to) break;
        longest = std::max(longest, std::chrono::duration_cast<std::chrono::microseconds>(reply.at - previous));
        previous = reply.at;
      }
      return std::max(longest, std::chrono::duration_cast<std::chrono::microseconds>(to - previous));
    }

    std::size_t
    count(tp from, tp to) const {
      return static_cast<std::size_t>(std::count_if(replies.begin(), replies.end(), [&](const reply_t &r) { return r.at >= from && r.at < to; }));
    }
  };

  std::vector<std::pair<tp, report_change>>
  train(tp from, tp to, int hz, report_change kind = report_change::stream) {
    std::vector<std::pair<tp, report_change>> events;
    for (auto at = from; at < to; at += std::chrono::microseconds(1000000 / hz)) events.emplace_back(at, kind);
    return events;
  }

  std::vector<std::pair<tp, report_change>>
  merged(std::vector<std::pair<tp, report_change>> a, const std::vector<std::pair<tp, report_change>> &b) {
    a.insert(a.end(), b.begin(), b.end());
    std::stable_sort(a.begin(), a.end(), [](const auto &x, const auto &y) { return x.first < y.first; });
    return a;
  }
}  // namespace

TEST(InterruptLane, FirstUrbIsAnsweredAtOnce) {
  lane_t lane;
  lane.hold(1, 7);
  const auto taken = lane.take_if_due(t0, 1);
  ASSERT_TRUE(taken.has_value());
  EXPECT_EQ(taken->item, 7);
  EXPECT_FALSE(taken->edge);
}

TEST(InterruptLane, NothingHeldMeansNothingDue) {
  lane_t lane;
  EXPECT_FALSE(lane.next_due(1).has_value());
  EXPECT_FALSE(lane.take_if_due(t0, 1).has_value());
}

TEST(InterruptLane, IdleKeepsTheFourMillisecondHeartbeat) {
  lane_t lane;
  lane.hold(1, 1);
  ASSERT_TRUE(lane.take_if_due(t0, 1));
  lane.hold(2, 2);
  EXPECT_FALSE(lane.take_if_due(t0 + 3999us, 1));
  EXPECT_TRUE(lane.take_if_due(t0 + 4ms, 1));
  lane.hold(3, 3);
  EXPECT_EQ(lane.next_due(1), t0 + 8ms);
}

TEST(InterruptLane, StreamDataAnswersAHeldUrbAfterTheOneMillisecondFloor) {
  lane_t lane;
  lane.hold(1, 1);
  ASSERT_TRUE(lane.take_if_due(t0, 1));
  lane.hold(2, 2);
  lane.new_data(report_change::stream);
  EXPECT_EQ(lane.next_due(1), t0 + 1ms);
  EXPECT_FALSE(lane.take_if_due(t0 + 999us, 1));
  EXPECT_TRUE(lane.take_if_due(t0 + 1ms, 1));
}

TEST(InterruptLane, DataBeforeTheNextUrbAnswersItAtOnceAfterTheFloor) {
  lane_t lane;
  lane.hold(1, 1);
  ASSERT_TRUE(lane.take_if_due(t0, 1));
  lane.new_data(report_change::stream);
  lane.hold(2, 2);
  EXPECT_EQ(lane.next_due(1), t0 + 1ms);
}

TEST(InterruptLane, SendingClearsTheDirtyFlag) {
  lane_t lane;
  lane.hold(1, 1);
  lane.new_data(report_change::stream);
  ASSERT_TRUE(lane.take_if_due(t0, 1));
  EXPECT_FALSE(lane.dirty());
  lane.hold(2, 2);
  EXPECT_EQ(lane.next_due(1), t0 + 4ms);
}

TEST(InterruptLane, ALateTimerNeverBringsTwoRepliesCloserThanTheFloor) {
  lane_t lane;
  lane.hold(1, 1);
  ASSERT_TRUE(lane.take_if_due(t0 + 3ms, 1));  // the timer fired late
  lane.hold(2, 2);
  lane.new_data(report_change::stream);
  EXPECT_EQ(lane.next_due(1), t0 + 4ms);
}

TEST(InterruptLane, RepliesHeldTogetherLeaveOneIntervalApart) {
  lane_t lane;
  for (std::uint32_t i = 1; i <= 3; ++i) lane.hold(i, static_cast<int>(i));
  EXPECT_EQ(lane.take_if_due(t0, 1)->item, 1);
  EXPECT_FALSE(lane.take_if_due(t0 + 1ms, 1));
  EXPECT_EQ(lane.take_if_due(t0 + 4ms, 1)->item, 2);
  EXPECT_EQ(lane.take_if_due(t0 + 8ms, 1)->item, 3);
}

TEST(InterruptLane, CancelRemovesOnlyAHeldReply) {
  lane_t lane;
  lane.hold(1, 1);
  lane.hold(2, 2);
  EXPECT_TRUE(lane.cancel(2));
  EXPECT_FALSE(lane.cancel(2));
  EXPECT_EQ(lane.held(), 1u);
  ASSERT_TRUE(lane.take_if_due(t0, 1));
  EXPECT_FALSE(lane.cancel(1));
}

TEST(InterruptLane, ResetForgetsTheClock) {
  lane_t lane;
  lane.hold(1, 1);
  lane.new_data(report_change::stream);
  lane.new_data(report_change::edge);
  ASSERT_TRUE(lane.take_if_due(t0, 1));
  lane.hold(2, 2);
  lane.reset();
  EXPECT_EQ(lane.held(), 0u);
  EXPECT_FALSE(lane.dirty());
  EXPECT_EQ(lane.pending_edges(), 0u);
  lane.hold(3, 3);
  EXPECT_TRUE(lane.take_if_due(t0 + 1us, 1));
}

TEST(InterruptLane, ConfiguredPeriodsApply) {
  lane_t::config settings;
  settings.min_interval = 2ms;
  settings.heartbeat = 10ms;
  lane_t lane { settings };
  lane.hold(1, 1);
  ASSERT_TRUE(lane.take_if_due(t0, 1));
  lane.hold(2, 2);
  EXPECT_EQ(lane.next_due(1), t0 + 10ms);
  lane.new_data(report_change::stream);
  EXPECT_EQ(lane.next_due(1), t0 + 2ms);
}

/* The heartbeat must go on between sparse events: a game that presses buttons at 125 Hz still
 * has the heartbeat underneath, whatever the number of URBs the host keeps outstanding. */
TEST(InterruptLane, HeartbeatContinuesBetweenSparseStreamEvents) {
  for (const int outstanding : { 1, 2, 4, 8, 32 }) {
    host_t host { outstanding };
    host.run(t0, t0 + 6s, train(t0, t0 + 6s, 125));
    EXPECT_LE(host.longest_silence(t0, t0 + 6s), 4100us) << "outstanding " << outstanding;
    EXPECT_GE(host.count(t0 + 1s, t0 + 6s), 5u * 250u - 5u) << "outstanding " << outstanding;
  }
}

/* A second of 500 Hz events, then quiet: the heartbeat resumes within one period. */
TEST(InterruptLane, HeartbeatResumesWithinFourMillisecondsAfterABurst) {
  for (const int outstanding : { 1, 2, 4, 8, 32 }) {
    host_t host { outstanding };
    host.run(t0, t0 + 4s, train(t0 + 500ms, t0 + 1500ms, 500));
    EXPECT_LE(host.longest_silence(t0, t0 + 4s), 4100us) << "outstanding " << outstanding;
  }
}

TEST(InterruptLane, StreamEventsAnswerAtTheirOwnRateAndNeverCloserThanTheFloor) {
  host_t host { 2 };
  host.run(t0, t0 + 2s, train(t0, t0 + 2s, 500));
  for (std::size_t i = 1; i < host.replies.size(); ++i) {
    EXPECT_GE(host.replies[i].at - host.replies[i - 1].at, 1ms) << "reply " << i;
  }
  EXPECT_NEAR(static_cast<double>(host.count(t0 + 500ms, t0 + 1500ms)), 500.0, 5.0);
}

TEST(InterruptLane, IdleReplyRateIsTheHeartbeat) {
  host_t host { 3 };
  host.run(t0, t0 + 3s, {});
  EXPECT_NEAR(static_cast<double>(host.count(t0 + 1s, t0 + 2s)), 250.0, 2.0);
}

/* The reply budget: every pad answers at 2000 / pads at most while a stream moves. */
TEST(InterruptLane, TheStreamFloorGrowsWithTheNumberOfActivePads) {
  lane_t lane;
  EXPECT_EQ(lane.stream_floor(1), 1ms);
  EXPECT_EQ(lane.stream_floor(0), 1ms);
  EXPECT_EQ(lane.stream_floor(2), 1ms);
  EXPECT_EQ(lane.stream_floor(4), 2ms);
  EXPECT_EQ(lane.stream_floor(8), 4ms);
  EXPECT_EQ(lane.stream_floor(16), 8ms);
}

TEST(InterruptLane, FourPadsTogetherStayWithinTheBudget) {
  host_t one { 2, 1 };
  host_t four { 2, 4 };
  one.run(t0, t0 + 3s, train(t0, t0 + 3s, 1000));
  four.run(t0, t0 + 3s, train(t0, t0 + 3s, 1000));
  const auto one_rate = static_cast<double>(one.count(t0 + 1s, t0 + 2s));
  const auto four_rate = static_cast<double>(four.count(t0 + 1s, t0 + 2s)) * 4.0;
  EXPECT_NEAR(one_rate, 1000.0, 10.0);
  EXPECT_LE(four_rate, 2000.0 * 1.02);
  EXPECT_GE(four_rate, 2000.0 * 0.95);
}

TEST(InterruptLane, TheBudgetNeverSlowsTheHeartbeat) {
  host_t host { 2, 8 };
  host.run(t0, t0 + 2s, {});
  EXPECT_NEAR(static_cast<double>(host.count(t0 + 500ms, t0 + 1500ms)), 250.0, 2.0);
}

/* Buttons. */
TEST(InterruptLane, EachButtonChangeGetsItsOwnReply) {
  lane_t lane;
  lane.hold(1, 1);
  ASSERT_TRUE(lane.take_if_due(t0, 1));
  for (std::uint32_t i = 2; i <= 4; ++i) lane.hold(i, static_cast<int>(i));
  lane.new_data(report_change::edge);  // press
  lane.new_data(report_change::edge);  // release 100 us later
  EXPECT_EQ(lane.pending_edges(), 2u);
  const auto first = lane.take_if_due(t0 + 1ms, 1);
  ASSERT_TRUE(first.has_value());
  EXPECT_TRUE(first->edge);
  EXPECT_FALSE(lane.take_if_due(t0 + 1999us, 1));
  const auto second = lane.take_if_due(t0 + 2ms, 1);
  ASSERT_TRUE(second.has_value());
  EXPECT_TRUE(second->edge);
  EXPECT_EQ(lane.pending_edges(), 0u);
}

TEST(InterruptLane, AButtonReplyIgnoresTheStreamFloor) {
  lane_t lane;
  lane.hold(1, 1);
  ASSERT_TRUE(lane.take_if_due(t0, 8));
  lane.hold(2, 2);
  lane.new_data(report_change::stream);
  EXPECT_EQ(lane.next_due(8), t0 + 4ms);  // eight pads: 4 ms
  lane.new_data(report_change::edge);
  EXPECT_EQ(lane.next_due(8), t0 + 1ms);
}

TEST(InterruptLane, AButtonReplyLeavesTheStreamChangeForTheNextReply) {
  lane_t lane;
  for (std::uint32_t i = 1; i <= 3; ++i) lane.hold(i, static_cast<int>(i));
  ASSERT_TRUE(lane.take_if_due(t0, 1));
  lane.new_data(report_change::stream);
  lane.new_data(report_change::edge);
  ASSERT_TRUE(lane.take_if_due(t0 + 1ms, 1)->edge);
  EXPECT_TRUE(lane.dirty());
  const auto next = lane.take_if_due(t0 + 20ms, 1);
  ASSERT_TRUE(next.has_value());
  EXPECT_FALSE(next->edge);
  EXPECT_FALSE(lane.dirty());
}

TEST(InterruptLane, ManyButtonChangesAtOnceAreCappedAndTheRestFallBackToTheStream) {
  lane_t lane;
  for (int i = 0; i < 12; ++i) lane.new_data(report_change::edge);
  EXPECT_EQ(lane.pending_edges(), lane_t::kMaxPendingEdges);
  EXPECT_TRUE(lane.dirty());
}

/* The HID class of Windows keeps 32 reports per reader.  With 500 Hz sticks and a press, a
 * reader that stalls must still find the press when it comes back, as long as it did with the old
 * fixed 4 ms pacing (32 x 4 ms = 128 ms). */
TEST(InterruptLane, AButtonReplySurvivesTheHidQueueAsLongAsWithTheFixedPacing) {
  for (const auto stall : { 40ms, 80ms, 120ms, 127ms }) {
    host_t host { 2 };
    const auto press = t0 + 1s;
    auto events = merged(train(t0, t0 + 3s, 500), { { press, report_change::edge }, { press + 30ms, report_change::edge } });
    host.run(t0, t0 + 3s, events);
    /* the reader stalls from the press until press + stall; it then sees the newest 32 replies */
    const auto stall_end = press + stall;
    std::deque<reply_t> queue;
    for (const auto &reply : host.replies) {
      if (reply.at >= press && reply.at <= stall_end) {
        queue.push_back(reply);
        if (queue.size() > 32) queue.pop_front();
      }
    }
    /* edge reply 1 is the press, edge reply 2 the release 30 ms later: the press itself must survive */
    const auto press_there = std::any_of(queue.begin(), queue.end(), [](const reply_t &r) { return r.edge_number == 1; });
    EXPECT_TRUE(press_there) << "stall " << stall.count() << " ms";
  }
}

/* Without the protection the same press would be gone after 64 ms of 500 Hz replies. */
TEST(InterruptLane, WithoutTheProtectionAFastStreamPushesAPressOutOfTheQueue) {
  lane_t::config settings;
  settings.protect_window = 0ms;
  host_t host { 2 };
  host.lane = lane_t { settings };
  host.lane.hold(100, 0);
  host.lane.hold(101, 0);
  const auto press = t0 + 1s;
  host.run(t0, t0 + 3s, merged(train(t0, t0 + 3s, 500), { { press, report_change::edge } }));
  std::deque<reply_t> queue;
  for (const auto &reply : host.replies) {
    if (reply.at >= press && reply.at <= press + 100ms) {
      queue.push_back(reply);
      if (queue.size() > 32) queue.pop_front();
    }
  }
  EXPECT_FALSE(std::any_of(queue.begin(), queue.end(), [](const reply_t &r) { return r.edge_number == 1; }));
}

TEST(InterruptLane, TheProtectionWindowKeepsTheFixedRateAndThenEnds) {
  host_t host { 2 };
  const auto press = t0 + 1s;
  host.run(t0, t0 + 3s, merged(train(t0, t0 + 3s, 500), { { press, report_change::edge } }));
  /* within the window: at most one reply per 4.5 ms, plus the button reply */
  EXPECT_LE(host.count(press, press + 144ms), 34u);
  /* after the window: back to the stream rate */
  EXPECT_NEAR(static_cast<double>(host.count(press + 300ms, press + 800ms)), 250.0, 5.0);
  /* the heartbeat never stops, protection or not */
  EXPECT_LE(host.longest_silence(t0, t0 + 3s), 4600us);
}

TEST(InterruptLane, StreamRepliesWaitTheProtectIntervalWhileTheProtectionLastsAndTheFloorAfterwards) {
  lane_t lane;
  lane.hold(1, 1);
  lane.new_data(report_change::edge);
  ASSERT_TRUE(lane.take_if_due(t0, 1)->edge);
  lane.hold(2, 2);
  lane.new_data(report_change::stream);
  EXPECT_EQ(lane.next_due(1), t0 + 4500us);  // not 1 ms
  ASSERT_TRUE(lane.take_if_due(t0 + 4500us, 1));
  /* the window ends 144 ms after the button reply; a reply 141 ms in is followed at the window's end */
  lane.hold(3, 3);
  lane.new_data(report_change::stream);
  ASSERT_TRUE(lane.take_if_due(t0 + 141ms, 1));
  lane.hold(4, 4);
  lane.new_data(report_change::stream);
  EXPECT_EQ(lane.next_due(1), t0 + 144ms);
  ASSERT_TRUE(lane.take_if_due(t0 + 144ms, 1));
  lane.hold(5, 5);
  lane.new_data(report_change::stream);
  EXPECT_EQ(lane.next_due(1), t0 + 145ms);  // the floor again
}

/* Timers are never exact: a heartbeat that leaves a little late must not push the next one back. */
TEST(InterruptLane, TheHeartbeatStaysOnItsGridWhenTheTimerIsLate) {
  lane_t lane;
  lane.hold(1, 1);
  ASSERT_TRUE(lane.take_if_due(t0, 1));
  lane.hold(2, 2);
  ASSERT_TRUE(lane.take_if_due(t0 + 4ms + 70us, 1));  // 70 us late
  lane.hold(3, 3);
  EXPECT_EQ(lane.next_due(1), t0 + 8ms);
}

TEST(InterruptLane, AHeartbeatThatIsVeryLateStartsANewGrid) {
  lane_t lane;
  lane.hold(1, 1);
  ASSERT_TRUE(lane.take_if_due(t0, 1));
  lane.hold(2, 2);
  ASSERT_TRUE(lane.take_if_due(t0 + 20ms, 1));  // 16 ms late
  lane.hold(3, 3);
  EXPECT_EQ(lane.next_due(1), t0 + 24ms);
}

TEST(InterruptLane, ALateHeartbeatNeverLeavesTwoRepliesCloserThanTheFloor) {
  lane_t lane;
  lane.hold(1, 1);
  ASSERT_TRUE(lane.take_if_due(t0, 1));
  lane.hold(2, 2);
  ASSERT_TRUE(lane.take_if_due(t0 + 7ms, 1));  // 3 ms late: the next one is due 1 ms later
  lane.hold(3, 3);
  EXPECT_GE(*lane.next_due(1) - (t0 + 7ms), 1ms);
}

TEST(InterruptLane, IdleRateDoesNotDriftWithTimerLateness) {
  lane_t lane;
  const auto late = 70us;
  tp now = t0;
  lane.hold(1, 1);
  ASSERT_TRUE(lane.take_if_due(now, 1));
  std::size_t replies = 0;
  for (int i = 0; i < 1000; ++i) {
    lane.hold(2, 2);
    now = *lane.next_due(1) + late;
    ASSERT_TRUE(lane.take_if_due(now, 1));
    ++replies;
  }
  /* 1000 replies on a 4 ms grid take 4000 ms plus one lateness, not 1000 latenesses */
  EXPECT_LE(now - t0, 4s + 200us);
  EXPECT_EQ(replies, 1000u);
}

/* A late heartbeat keeps the grid, but the floor between two replies counts from when it really left. */
TEST(InterruptLane, AStreamChangeAfterALateHeartbeatStillWaitsTheFloorFromTheRealSendTime) {
  lane_t lane;
  lane.hold(1, 1);
  ASSERT_TRUE(lane.take_if_due(t0, 1));
  lane.hold(2, 2);
  const auto sent = t0 + 4ms + 2500us;  // due at 4 ms, left 2.5 ms late
  ASSERT_TRUE(lane.take_if_due(sent, 1));
  lane.hold(3, 3);
  lane.new_data(report_change::stream);
  EXPECT_GE(*lane.next_due(1) - sent, 1ms);
  EXPECT_FALSE(lane.take_if_due(sent + 999us, 1));
}

TEST(InterruptLane, AButtonChangeAfterALateHeartbeatStillWaitsTheFloorFromTheRealSendTime) {
  lane_t lane;
  lane.hold(1, 1);
  ASSERT_TRUE(lane.take_if_due(t0, 1));
  lane.hold(2, 2);
  const auto sent = t0 + 4ms + 2500us;
  ASSERT_TRUE(lane.take_if_due(sent, 1));
  lane.hold(3, 3);
  lane.new_data(report_change::edge);
  EXPECT_GE(*lane.next_due(1) - sent, 1ms);
}

TEST(InterruptLane, TheNextHeartbeatAfterALateOneIsStillOnTheGrid) {
  lane_t lane;
  lane.hold(1, 1);
  ASSERT_TRUE(lane.take_if_due(t0, 1));
  lane.hold(2, 2);
  ASSERT_TRUE(lane.take_if_due(t0 + 4ms + 2500us, 1));
  lane.hold(3, 3);
  EXPECT_EQ(lane.next_due(1), t0 + 8ms);
}
