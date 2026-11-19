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

/* The reply budget: one or two pads keep the hard floor; with more, every pad answers at 1500 / pads at most while a stream moves. */
TEST(InterruptLane, TheStreamFloorGrowsWithTheNumberOfActivePads) {
  lane_t lane;
  EXPECT_EQ(lane.stream_floor(1), 1ms);
  EXPECT_EQ(lane.stream_floor(0), 1ms);
  EXPECT_EQ(lane.stream_floor(2), 1ms);
  EXPECT_EQ(lane.stream_floor(3), 2ms);
  EXPECT_EQ(std::chrono::duration_cast<std::chrono::microseconds>(lane.stream_floor(4)), 2666us);
}

TEST(InterruptLane, TheStreamFloorNeverExceedsTheHeartbeat) {
  lane_t lane;
  EXPECT_EQ(lane.stream_floor(8), 4ms);
  EXPECT_EQ(lane.stream_floor(16), 4ms);
}

TEST(InterruptLane, FourPadsTogetherStayWithinTheBudget) {
  host_t one { 2, 1 };
  host_t four { 2, 4 };
  one.run(t0, t0 + 3s, train(t0, t0 + 3s, 1000));
  four.run(t0, t0 + 3s, train(t0, t0 + 3s, 1000));
  const auto one_rate = static_cast<double>(one.count(t0 + 1s, t0 + 2s));
  const auto four_rate = static_cast<double>(four.count(t0 + 1s, t0 + 2s)) * 4.0;
  EXPECT_NEAR(one_rate, 1000.0, 10.0);
  EXPECT_LE(four_rate, 1500.0 * 1.02);
  EXPECT_GE(four_rate, 1500.0 * 0.95);
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

/* The HID class of Windows keeps 32 reports per reader.  A reader that stalls must still find a
 * report that shows a short press when it comes back, as long as it did with the old fixed 4 ms
 * pacing (32 x 4 ms = 128 ms).  While the button is held every report shows it, so what counts is
 * the last report before the release. */
namespace {
  struct tap_run_t {
    host_t host { 2 };
    tp press, release;
    std::size_t first_down {}, release_reply {};  ///< indexes into host.replies
  };

  tap_run_t
  run_tap(lane_t::config settings, std::chrono::microseconds hold, int stick_hz = 500, unsigned outstanding_pads = 1) {
    tap_run_t run;
    run.host.lane = lane_t { settings };
    run.host.lane.hold(100, 0);
    run.host.lane.hold(101, 0);
    run.host.pads = outstanding_pads;
    run.press = t0 + 1s;
    run.release = run.press + hold;
    run.host.run(t0, t0 + 3s, merged(train(t0, t0 + 3s, stick_hz), { { run.press, report_change::edge }, { run.release, report_change::release } }));
    for (std::size_t i = 0; i != run.host.replies.size(); ++i) {
      if (run.host.replies[i].edge_number == 1) run.first_down = i;
      if (run.host.replies[i].edge_number == 2) run.release_reply = i;
    }
    return run;
  }

  /** Is a report that shows the button down among the newest 32 when a reader that stalled since the press comes back at @p stall_end? */
  bool
  down_report_in_queue(const tap_run_t &run, tp stall_end) {
    std::deque<std::size_t> queue;
    for (std::size_t i = run.first_down; i != run.host.replies.size() && run.host.replies[i].at <= stall_end; ++i) {
      queue.push_back(i);
      if (queue.size() > 32) queue.pop_front();
    }
    return std::any_of(queue.begin(), queue.end(), [&](std::size_t i) { return i >= run.first_down && i < run.release_reply; });
  }
}  // namespace

TEST(InterruptLane, TheLastReportThatShowedAButtonDownSurvivesTheHidQueueAsLongAsWithTheFixedPacing) {
  for (const auto hold : { 2ms, 10ms, 30ms, 70ms, 150ms }) {
    const auto run = run_tap({}, hold);
    ASSERT_GT(run.release_reply, run.first_down);
    /* the reader stalls from the press; 120 ms after the release it is back */
    for (const auto after_release : { 20ms, 60ms, 100ms, 119ms }) {
      EXPECT_TRUE(down_report_in_queue(run, run.release + after_release)) << "hold " << hold.count() << " ms, back " << after_release.count() << " ms after the release";
    }
  }
}

/* Without the protection a fast stream pushes it out of the queue within 32 ms of the release. */
TEST(InterruptLane, WithoutTheProtectionAFastStreamPushesTheDownReportOutOfTheQueue) {
  lane_t::config settings;
  settings.protect_window = 0ms;
  const auto run = run_tap(settings, 30ms);
  EXPECT_FALSE(down_report_in_queue(run, run.release + 100ms));
}

TEST(InterruptLane, APressAloneNeedsNoProtection) {
  host_t host { 2 };
  const auto press = t0 + 1s;
  host.run(t0, t0 + 3s, merged(train(t0, t0 + 3s, 500), { { press, report_change::edge } }));
  /* the button stays down: the stream keeps its 500 Hz right through the press */
  EXPECT_GE(host.count(press + 10ms, press + 110ms), 48u);
  EXPECT_LE(host.longest_silence(t0, t0 + 3s), 2100us);
}

TEST(InterruptLane, TheProtectionAfterAReleaseKeepsItsGridAndThenEnds) {
  host_t host { 2 };
  const auto release = t0 + 1s;
  host.run(t0, t0 + 3s, merged(train(t0, t0 + 3s, 500), { { release, report_change::release } }));
  /* within the window: one reply per 4.5 ms on the grid, plus the release reply */
  EXPECT_LE(host.count(release, release + 140ms), 32u);
  EXPECT_GE(host.count(release, release + 140ms), 30u);
  /* after the window: back to the stream rate */
  EXPECT_NEAR(static_cast<double>(host.count(release + 300ms, release + 800ms)), 250.0, 5.0);
  /* the heartbeat never stops, protection or not (it follows the protection's grid while that lasts) */
  EXPECT_LE(host.longest_silence(t0, t0 + 3s), 4600us);
}

/* Timers are never exact: the protected replies must not drift by the lateness of every timer. */
TEST(InterruptLane, TheProtectedRepliesStayOnTheirGridWhenTheTimerIsLate) {
  lane_t lane;
  lane.hold(1, 1);
  lane.new_data(report_change::release);
  ASSERT_TRUE(lane.take_if_due(t0, 1)->edge);
  tp now = t0;
  for (std::uint32_t i = 2; i != 30; ++i) {
    lane.hold(i, 1);
    lane.new_data(report_change::stream);
    const auto due = lane.next_due(1);
    ASSERT_TRUE(due.has_value());
    now = *due + 250us;  // every timer fires 250 us late
    ASSERT_TRUE(lane.take_if_due(now, 1));
  }
  /* 28 protected replies after the release at t0: the grid has kept 4.5 ms apart, not 4.75 ms */
  EXPECT_LE(now, t0 + 28 * 4500us + 250us);
}

TEST(InterruptLane, AReleaseThatFindsNoRoomOfItsOwnStillStartsTheProtectionWithTheNextReply) {
  lane_t lane;
  for (std::uint32_t i = 1; i <= 14; ++i) lane.hold(i, static_cast<int>(i));
  ASSERT_TRUE(lane.take_if_due(t0, 1));
  for (unsigned i = 0; i < lane_t::kMaxPendingEdges; ++i) lane.new_data(report_change::edge);
  lane.new_data(report_change::release);  // the ninth change: no room, falls back to the stream
  EXPECT_EQ(lane.pending_edges(), lane_t::kMaxPendingEdges);
  tp now = t0;
  for (unsigned i = 0; i < lane_t::kMaxPendingEdges; ++i) {
    now += 1ms;
    ASSERT_TRUE(lane.take_if_due(now, 1)->edge);
  }
  now += 1ms;
  ASSERT_FALSE(lane.take_if_due(now, 1)->edge);  // the stream reply that carries the release
  lane.new_data(report_change::stream);
  EXPECT_EQ(lane.next_due(1), now + 4500us);  // protected
}

TEST(InterruptLane, ShortTapsUnderAFastStreamKeepEveryPressVisibleToAStalledReader) {
  /* a 30 ms tap every 70 ms for two seconds; after each release a reader that was away since the press is back within 120 ms */
  host_t host { 2 };
  std::vector<std::pair<tp, report_change>> taps;
  for (int i = 0; i < 25; ++i) {
    taps.emplace_back(t0 + 100ms + i * 70ms, report_change::edge);
    taps.emplace_back(t0 + 130ms + i * 70ms, report_change::release);
  }
  host.run(t0, t0 + 3s, merged(train(t0, t0 + 3s, 500), taps));
  std::vector<std::size_t> presses, releases;
  for (std::size_t i = 0; i != host.replies.size(); ++i) {
    if (!host.replies[i].edge) continue;
    (presses.size() == releases.size() ? presses : releases).push_back(i);
  }
  ASSERT_EQ(presses.size(), releases.size());
  ASSERT_EQ(presses.size(), 25u);
  for (std::size_t tap = 0; tap != presses.size(); ++tap) {
    /* the reader is back 100 ms after this release: the newest 32 replies up to then */
    const auto back = host.replies[releases[tap]].at + 100ms;
    std::deque<std::size_t> queue;
    for (std::size_t i = presses[tap]; i != host.replies.size() && host.replies[i].at <= back; ++i) {
      queue.push_back(i);
      if (queue.size() > 32) queue.pop_front();
    }
    EXPECT_TRUE(std::any_of(queue.begin(), queue.end(), [&](std::size_t i) { return i >= presses[tap] && i < releases[tap]; })) << "tap " << tap;
  }
}

TEST(InterruptLane, StreamRepliesWaitTheProtectIntervalWhileTheProtectionLastsAndTheFloorAfterwards) {
  lane_t lane;
  lane.hold(1, 1);
  lane.new_data(report_change::release);
  ASSERT_TRUE(lane.take_if_due(t0, 1)->edge);
  lane.hold(2, 2);
  lane.new_data(report_change::stream);
  EXPECT_EQ(lane.next_due(1), t0 + 4500us);  // not 1 ms
  ASSERT_TRUE(lane.take_if_due(t0 + 4500us, 1));
  /* the window ends 140 ms after the release reply; a reply 137 ms in is followed at the window's end */
  lane.hold(3, 3);
  lane.new_data(report_change::stream);
  ASSERT_TRUE(lane.take_if_due(t0 + 137ms, 1));
  lane.hold(4, 4);
  lane.new_data(report_change::stream);
  EXPECT_EQ(lane.next_due(1), t0 + 140ms);
  ASSERT_TRUE(lane.take_if_due(t0 + 140ms, 1));
  lane.hold(5, 5);
  lane.new_data(report_change::stream);
  EXPECT_EQ(lane.next_due(1), t0 + 141ms);  // the floor again
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

/* A burst of presses (releases less than 32 ms apart) keeps the 4 ms pacing; a single press gets the longer one. */
TEST(InterruptLane, AReleaseSoonAfterAnotherOneKeepsTheOldFourMillisecondPacing) {
  lane_t lane;
  for (std::uint32_t i = 1; i <= 8; ++i) lane.hold(i, static_cast<int>(i));
  lane.new_data(report_change::release);
  ASSERT_TRUE(lane.take_if_due(t0, 1)->edge);
  lane.new_data(report_change::stream);
  EXPECT_EQ(lane.next_due(1), t0 + 4500us);
  ASSERT_TRUE(lane.take_if_due(t0 + 4500us, 1));
  /* the next release comes 20 ms after the first: a burst */
  lane.new_data(report_change::release);
  ASSERT_TRUE(lane.take_if_due(t0 + 20ms, 1)->edge);
  lane.new_data(report_change::stream);
  EXPECT_EQ(lane.next_due(1), t0 + 24ms);
}

TEST(InterruptLane, AReleaseWellAfterAnotherOneGetsTheLongerProtection) {
  lane_t lane;
  for (std::uint32_t i = 1; i <= 8; ++i) lane.hold(i, static_cast<int>(i));
  lane.new_data(report_change::release);
  ASSERT_TRUE(lane.take_if_due(t0, 1)->edge);
  lane.new_data(report_change::release);
  ASSERT_TRUE(lane.take_if_due(t0 + 70ms, 1)->edge);
  lane.new_data(report_change::stream);
  EXPECT_EQ(lane.next_due(1), t0 + 70ms + 4500us);
}

/* The burst protection keeps the 124 ms of the fixed 4 ms pacing: a reader that was away since the press of the last tap of a burst
 * still finds a report that shows it down when it is back 123 ms after the release. */
TEST(InterruptLane, ABurstOfPressesKeepsTheLastDownReportFor124Milliseconds) {
  host_t host { 2 };
  const auto press = t0 + 1s;
  host.run(t0, t0 + 3s,
    merged(train(t0, t0 + 3s, 500),
      { { press, report_change::edge }, { press + 10ms, report_change::release }, { press + 20ms, report_change::edge }, { press + 30ms, report_change::release } }));
  std::size_t second_press = 0, second_release = 0;
  for (std::size_t i = 0; i != host.replies.size(); ++i) {
    if (host.replies[i].edge_number == 3) second_press = i;
    if (host.replies[i].edge_number == 4) second_release = i;
  }
  ASSERT_GT(second_release, second_press);
  const auto back = host.replies[second_release].at + 123ms;
  std::deque<std::size_t> queue;
  for (std::size_t i = second_press; i != host.replies.size() && host.replies[i].at <= back; ++i) {
    queue.push_back(i);
    if (queue.size() > 32) queue.pop_front();
  }
  EXPECT_TRUE(std::any_of(queue.begin(), queue.end(), [&](std::size_t i) { return i >= second_press && i < second_release; }));
}

/* A press every 70 ms for 30 ms keeps the 139 ms the longer protection promises, even 160 ms stalls lose nothing. */
TEST(InterruptLane, ShortTapsSurviveAStallOf160MillisecondsThatStartsWithThePress) {
  for (const auto hold : { 30ms, 50ms, 80ms }) {
    const auto run = run_tap({}, hold);
    EXPECT_TRUE(down_report_in_queue(run, run.press + 160ms)) << "hold " << hold.count() << " ms";
  }
}

/* A lower rate asked for by the user (ds5.compat.inputReportRate): a floor longer than the hard one. */
namespace {
  std::chrono::microseconds
  shortest_gap(const host_t &host, tp from, tp to) {
    std::chrono::microseconds shortest { 1000000 };
    const reply_t *previous = nullptr;
    for (const auto &reply : host.replies) {
      if (reply.at < from || reply.at > to) continue;
      if (previous) shortest = std::min(shortest, std::chrono::duration_cast<std::chrono::microseconds>(reply.at - previous->at));
      previous = &reply;
    }
    return shortest;
  }
}  // namespace

TEST(InterruptLane, AFourMillisecondFloorAnswersChangesOnTheHeartbeatGrid) {
  host_t host { 2 };
  host.lane.set_floor(4ms);
  host.run(t0, t0 + 3s, train(t0, t0 + 3s, 500));
  EXPECT_GE(shortest_gap(host, t0 + 100ms, t0 + 3s), 3990us);
  EXPECT_NEAR(static_cast<double>(host.count(t0 + 1s, t0 + 2s)), 250.0, 4.0);
  EXPECT_LE(host.longest_silence(t0, t0 + 3s), 4100us);
}

TEST(InterruptLane, AFiveHundredHertzFloorAnswersEveryTwoMilliseconds) {
  host_t host { 2 };
  host.lane.set_floor(2ms);
  host.run(t0, t0 + 3s, train(t0, t0 + 3s, 1000));
  EXPECT_GE(shortest_gap(host, t0 + 100ms, t0 + 3s), 1990us);
  EXPECT_NEAR(static_cast<double>(host.count(t0 + 1s, t0 + 2s)), 500.0, 6.0);
}

TEST(InterruptLane, WithAFourMillisecondFloorButtonRepliesFollowTheFloorToo) {
  host_t host { 2 };
  host.lane.set_floor(4ms);
  const auto press = t0 + 1s;
  host.run(t0, t0 + 3s, merged(train(t0, t0 + 3s, 500), { { press, report_change::edge }, { press + 30ms, report_change::release } }));
  EXPECT_GE(shortest_gap(host, t0 + 100ms, t0 + 3s), 3990us) << "a press is answered on the grid like everything else";
}

TEST(InterruptLane, WithAFourMillisecondFloorAReleaseNeedsNoProtectionAndTheGridStaysFourMilliseconds) {
  lane_t lane;
  lane.set_floor(4ms);
  lane.hold(1, 1);
  lane.new_data(report_change::release);
  ASSERT_TRUE(lane.take_if_due(t0, 1)->edge);
  lane.hold(2, 2);
  lane.new_data(report_change::stream);
  EXPECT_EQ(lane.next_due(1), t0 + 4ms) << "not 4.5 ms";
}

TEST(InterruptLane, AShortPressSurvivesALongStallWithAFourMillisecondFloorWithoutTheProtection) {
  lane_t::config settings;
  settings.protect_window = 0ms;  // the protection is off in this run, as it is under the floor
  const auto run = run_tap(settings, 30ms);
  EXPECT_FALSE(down_report_in_queue(run, run.release + 100ms)) << "without a floor a fast stream pushes the press out";
  /* the same press with the floor: 32 replies are at least 128 ms */
  host_t host { 2 };
  host.lane.set_floor(4ms);
  const auto press = t0 + 1s;
  host.run(t0, t0 + 3s, merged(train(t0, t0 + 3s, 500), { { press, report_change::edge }, { press + 30ms, report_change::release } }));
  std::size_t press_reply = 0, release_reply = 0;
  for (std::size_t i = 0; i != host.replies.size(); ++i) {
    if (host.replies[i].edge_number == 1) press_reply = i;
    if (host.replies[i].edge_number == 2) release_reply = i;
  }
  ASSERT_GT(release_reply, press_reply);
  std::deque<std::size_t> queue;
  for (std::size_t i = press_reply; i != host.replies.size() && host.replies[i].at <= press + 30ms + 120ms; ++i) {
    queue.push_back(i);
    if (queue.size() > 32) queue.pop_front();
  }
  EXPECT_TRUE(std::any_of(queue.begin(), queue.end(), [&](std::size_t i) { return i >= press_reply && i < release_reply; }));
}

TEST(InterruptLane, ANoFloorLeavesTheLaneAsItWas) {
  lane_t lane;
  lane.set_floor(0ms);
  lane.hold(1, 1);
  ASSERT_TRUE(lane.take_if_due(t0, 1));
  lane.hold(2, 2);
  lane.new_data(report_change::stream);
  EXPECT_EQ(lane.next_due(1), t0 + 1ms);
}

TEST(InterruptLane, AFasterRateChosenRightAfterAReleaseFindsTheRestOfItsProtection) {
  lane_t lane;
  lane.set_floor(4ms);
  lane.hold(1, 1);
  lane.new_data(report_change::release);
  ASSERT_TRUE(lane.take_if_due(t0, 1)->edge);
  lane.set_floor(0ms);  // the rate setting moved to automatic inside the protection window
  lane.hold(2, 2);
  lane.new_data(report_change::stream);
  EXPECT_EQ(lane.next_due(1), t0 + 4500us) << "the protection of the release still runs: not 1 ms";
}
