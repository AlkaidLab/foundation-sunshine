#include "src/streaming/bitrate.h"
#include "src/streaming/fec.h"
#include <gtest/gtest.h>
#include <limits>
#include <memory>
#include <vector>

TEST(VideoFec, PreferenceParserRejectsInvalidAndOutOfRangeInput) {
  for (const auto input : { "", "4x", "-3", "101", "999999999999999999999", " 1", "+1" })
    EXPECT_FALSE(video_fec::parse_preference(input));
  EXPECT_EQ(video_fec::parse_preference("-2"), -2);
  EXPECT_EQ(video_fec::parse_preference("-1"), -1);
  EXPECT_EQ(video_fec::parse_preference("0"), 0);
  EXPECT_EQ(video_fec::parse_preference("100"), 100);
}

TEST(VideoFec, ClientFixedPreferenceOverridesFeedbackAndOtherSessions) {
  video_fec::controller_t first, second;
  first.initialize(20, true, 50);
  second.initialize(20, true, 50);
  ASSERT_TRUE(first.set_mode(video_fec::mode_e::fixed, 7));
  ASSERT_TRUE(first.report(1, 1000, 200, 1000));
  EXPECT_EQ(first.percentage(100, 2, 1000), 7);
  EXPECT_EQ(second.percentage(100, 2, 1000), 20);
  EXPECT_FALSE(first.set_mode(video_fec::mode_e::fixed, 101));
  EXPECT_EQ(first.percentage(100, 2, 1000), 7);
}

TEST(VideoFec, AutomaticSelectionRespondsToLossWithinCap) {
  video_fec::controller_t control;
  control.initialize(20, false, 50);
  ASSERT_TRUE(control.set_mode(video_fec::mode_e::automatic, 0));
  ASSERT_TRUE(control.report(1, 10000, 0, 1000));
  EXPECT_EQ(control.percentage(100, 2, 1000), 0);
  ASSERT_TRUE(control.report(2, 10000, 1000, 5000));
  const auto protection = control.percentage(100, 2, 5000);
  EXPECT_GT(protection, 0);
  EXPECT_LE(protection, 50);
  EXPECT_EQ(control.percentage(100, 2, 8001), 20);
  ASSERT_TRUE(control.set_mode(video_fec::mode_e::host, 0));
  EXPECT_EQ(control.percentage(100, 2, 5000), 20);
}

TEST(VideoFec, InvalidDuplicateAndOldFeedbackCannotChangeSelection) {
  video_fec::controller_t control;
  control.initialize(20, true, 50);
  EXPECT_FALSE(control.report(1, 0, 0, 1000));
  EXPECT_FALSE(control.report(1, 100, 101, 1000));
  ASSERT_TRUE(control.report(0xffffffffU, 1000, 0, 1000));
  EXPECT_FALSE(control.report(0xffffffffU, 1000, 1000, 1001));
  ASSERT_TRUE(control.report(0, 1000, 0, 2000));
  EXPECT_FALSE(control.report(0xfffffffeU, 1000, 1000, 2001));
  EXPECT_FALSE(control.report(1, 1000, 1000, 1999));
  EXPECT_EQ(control.percentage(200, 2, 2000), 0);
}

TEST(VideoFecWindow, WeightsPacketsWithinBucketsAndKeepsObservationsSeparate) {
  video_fec::loss_window_t window;
  EXPECT_FALSE(window.estimate(0));
  window.record(10, 1, 1000);
  window.record(990, 0, 1500);
  window.record(99000, 0, 2000);
  const auto estimate = window.estimate(2000);
  ASSERT_TRUE(estimate);
  EXPECT_DOUBLE_EQ(*estimate, 0.001);
}

TEST(VideoFecWindow, ExpiresHistoryAndDistinguishesCleanFromMissingFeedback) {
  video_fec::loss_window_t window;
  window.record(1000, 20, 0);
  window.record(1000, 0, 9000);
  ASSERT_TRUE(window.estimate(9999));
  EXPECT_DOUBLE_EQ(*window.estimate(9999), 0.02);
  const auto clean = window.estimate(10000);
  ASSERT_TRUE(clean);
  EXPECT_DOUBLE_EQ(*clean, 0);
  EXPECT_FALSE(window.estimate(19000));
  // Reusing a bucket replaces the old counts; large aggregates stay 64-bit.
  for (unsigned i = 0; i < 5000; ++i) window.record(1000000, 1, 20000);
  const auto reused = window.estimate(20000);
  ASSERT_TRUE(reused);
  EXPECT_DOUBLE_EQ(*reused, 0.000001);
}

TEST(VideoFec, FiniteCleanWindowReleasesProtectionAndRearmsAfterLoss) {
  video_fec::controller_t control;
  control.initialize(20, true, 50);
  ASSERT_TRUE(control.report(1, 10000, 300, 0));
  for (unsigned second = 1; second <= 9; ++second) {
    ASSERT_TRUE(control.report(second + 1, 10000, 0, second * 1000));
    EXPECT_GT(control.percentage(100, 2, second * 1000), 0);
  }
  ASSERT_TRUE(control.report(11, 10000, 0, 10000));
  EXPECT_EQ(control.percentage(100, 2, 10000), 0);
  const auto frame = video_fec::plan(100, control.percentage(100, 2, 10000), 2);
  ASSERT_TRUE(frame);
  EXPECT_EQ(frame->blocks[0].parity, 0);
  ASSERT_TRUE(control.report(12, 10000, 300, 11000));
  EXPECT_GT(control.percentage(100, 2, 11000), 0);
}

TEST(VideoFec, LossAndInvalidFeedbackCannotCompleteCleanRecovery) {
  video_fec::controller_t control;
  control.initialize(20, true, 50);
  ASSERT_TRUE(control.report(1, 10000, 300, 0));
  ASSERT_TRUE(control.report(2, 10000, 0, 1000));
  ASSERT_TRUE(control.report(3, 10000, 0, 3000));
  ASSERT_TRUE(control.report(4, 10000, 1, 5000));
  EXPECT_FALSE(control.report(4, 10000, 0, 6000));
  EXPECT_FALSE(control.report(3, 10000, 0, 6000));
  EXPECT_FALSE(control.report(5, 0, 0, 6000));
  for (unsigned second = 6; second <= 14; ++second) {
    ASSERT_TRUE(control.report(second - 1, 10000, 0, second * 1000));
    EXPECT_GT(control.percentage(100, 2, second * 1000), 0);
  }
  ASSERT_TRUE(control.report(14, 10000, 0, 15000));
  EXPECT_EQ(control.percentage(100, 2, 15000), 0);
  EXPECT_EQ(control.percentage(100, 2, 18001), 20);
  control.initialize(20, false, 50);
  EXPECT_EQ(control.percentage(100, 2, 15000), 20);
}

TEST(VideoFec, FeedbackTimeoutRestartsCleanRecoveryWithoutReleasingProtection) {
  video_fec::controller_t control;
  control.initialize(20, true, 50);
  ASSERT_TRUE(control.report(1, 10000, 300, 0));
  ASSERT_TRUE(control.report(2, 10000, 0, 1000));
  EXPECT_EQ(control.percentage(100, 2, 4001), 20);
  ASSERT_TRUE(control.report(3, 10000, 0, 4001));
  EXPECT_EQ(control.percentage(100, 2, 4001), 20);
  unsigned sequence = 4;
  for (const auto now : { 6001, 8001, 10000, 12000, 14000 }) {
    ASSERT_TRUE(control.report(sequence++, 10000, 0, now));
    EXPECT_EQ(control.percentage(100, 2, now), 20);
  }
  ASSERT_TRUE(control.report(9, 10000, 0, 14001));
  EXPECT_EQ(control.percentage(100, 2, 14001), 0);
  control.initialize(20, true, 50);
  ASSERT_TRUE(control.report(1, 10000, 0, 15000));
  EXPECT_EQ(control.percentage(100, 2, 15000), 0);
}

TEST(VideoFec, MissingSummaryRestartsObservationAndFixedZeroStillWins) {
  video_fec::controller_t control;
  control.initialize(20, true, 50);
  ASSERT_TRUE(control.report(1, 1000, 0, 0));
  EXPECT_EQ(control.percentage(100, 2, 0), 0);
  ASSERT_TRUE(control.report(3, 1000, 0, 1000));  // Report 2 was not received.
  EXPECT_EQ(control.percentage(100, 2, 1000), 20);
  ASSERT_TRUE(control.set_mode(video_fec::mode_e::fixed, 0));
  EXPECT_EQ(control.percentage(100, 2, 1000), 0);
  ASSERT_TRUE(control.set_mode(video_fec::mode_e::automatic, 0));
  for (unsigned second = 2; second <= 10; ++second) {
    ASSERT_TRUE(control.report(second + 2, 1000, 0, second * 1000));
    EXPECT_EQ(control.percentage(100, 2, second * 1000), 20);
  }
  ASSERT_TRUE(control.report(13, 1000, 0, 11000));
  EXPECT_EQ(control.percentage(100, 2, 11000), 0);
}

TEST(VideoFec, MinimumParityAndWirePercentageAgree) {
  for (unsigned data = 1; data <= 255; ++data) {
    for (unsigned ratio : { 1U, 20U, 50U, 100U }) {
      const auto block = video_fec::plan_block(data, ratio, 2);
      if (!block) continue;
      EXPECT_EQ(block->parity, (data * block->percentage + 99) / 100);
      EXPECT_GE(block->parity, 2);
      EXPECT_LE(block->data + block->parity, 255);
    }
  }
}

TEST(VideoFec, FramePlansPreserveDataAndProtocolBounds) {
  for (const auto data : { 1U, 50U, 200U, 300U, 600U, 800U, 1000U, 4092U }) {
    for (const auto ratio : { 0U, 20U, 50U, 100U }) {
      const auto frame = video_fec::plan(data, ratio, 2);
      ASSERT_TRUE(frame);
      unsigned actual_data = 0;
      ASSERT_LE(frame->count, 4);
      for (size_t i = 0; i < frame->count; ++i) {
        const auto &block = frame->blocks[i];
        actual_data += block.data;
        EXPECT_GT(block.data, 0);
        if (block.parity)
          EXPECT_LE(block.data + block.parity, 255);
        else
          EXPECT_LE(block.data, 1023);
      }
      EXPECT_EQ(actual_data, data);
    }
  }
  EXPECT_FALSE(video_fec::plan(0, 20, 2));
  EXPECT_FALSE(video_fec::plan(4093, 20, 2));
}

TEST(VideoFec, AutomaticCapCannotDisableRepresentableProtection) {
  video_fec::controller_t control;
  control.initialize(20, true, 50);
  ASSERT_TRUE(control.report(1, 1000, 200, 1000));
  const auto ratio = control.percentage(800, 2, 1000);
  EXPECT_GT(ratio, 0);
  EXPECT_LE(ratio, 50);
  const auto frame = video_fec::plan(800, ratio, 2);
  ASSERT_TRUE(frame);
  EXPECT_FALSE(frame->skipped);
}

TEST(VideoFecBudget, RepairRatioAndEncodingBudgetUseTheSameDenominator) {
  for (unsigned ratio = 0; ratio <= 255; ++ratio) {
    for (int total : { 1, 10000, 800000, std::numeric_limits<int>::max() }) {
      const auto encoder = streaming::encoder_bitrate(total, ratio);
      EXPECT_GT(encoder, 0);
      if (total >= 4) {
        EXPECT_LE(static_cast<std::int64_t>(encoder) * (100 + ratio), static_cast<std::int64_t>(total) * 100);
      }
      EXPECT_GE(streaming::total_bitrate(encoder, ratio), encoder);
    }
  }
  EXPECT_EQ(streaming::encoder_bitrate(10000, 0), 10000);
  EXPECT_EQ(streaming::encoder_bitrate(10000, 20), 8333);
  EXPECT_EQ(streaming::encoder_bitrate(10000, 100), 5000);
}

TEST(VideoFecBudget, FixedZeroAndTotalRequestsShareReservesAndHostCap) {
  streaming::bitrate_budget_t first(10000, 10000, 512, 500, 0, false);
  streaming::bitrate_budget_t second(10000, 10000, 512, 500, 100, false);
  EXPECT_EQ(first.applied().encoder_kbps, 8988);
  EXPECT_EQ(second.applied().encoder_kbps, 4494);
  first.observe_fec(100, 0);
  first.observe_fec(100, 1000);
  EXPECT_FALSE(first.pending(1000));
  EXPECT_EQ(first.request(20000), 10000);
  EXPECT_FALSE(first.pending(1001));
  EXPECT_EQ(first.request(5000), 5000);
  auto next = first.pending(1002);
  ASSERT_TRUE(next);
  EXPECT_EQ(next->encoder_kbps, 4040);
  EXPECT_EQ(next->fec_percentage, 0);
  EXPECT_EQ(second.total(), 10000);
  first.commit(*next);
  EXPECT_FALSE(first.pending(1003));
}

TEST(VideoFecBudget, AutomaticWindowsDoNotRaiseProtectionBeforeEncoderSuccess) {
  streaming::bitrate_budget_t budget(10000, 0, 0, 0, 20, true);
  budget.observe_fec(50, 0);
  budget.observe_fec(30, 500);
  EXPECT_FALSE(budget.pending(999));
  budget.observe_fec(50, 1000);
  auto next = budget.pending(1000);
  ASSERT_TRUE(next);
  EXPECT_EQ(next->fec_percentage, 50);
  EXPECT_EQ(next->encoder_kbps, 6666);
  // Rejecting an encoder update keeps the previous allowance and rate.
  EXPECT_EQ(budget.applied().fec_percentage, 20);
  EXPECT_FALSE(budget.pending(1999));
  ASSERT_TRUE(budget.pending(2000));
  budget.commit(*next);
  EXPECT_EQ(budget.applied().fec_percentage, 50);
  // The sender can lower protection immediately; recovering the encoding
  // budget waits for a completed clean control window.
  budget.observe_fec(0, 2000);
  budget.observe_fec(0, 3000);
  next = budget.pending(3000);
  ASSERT_TRUE(next);
  EXPECT_EQ(next->fec_percentage, 0);
  EXPECT_EQ(next->encoder_kbps, 10000);
  budget.commit(*next);
  EXPECT_EQ(budget.total(), 10000);
}

TEST(VideoFecBudget, ConcurrentBitrateRequestSurvivesEncoderCommitAndReinit) {
  streaming::bitrate_budget_t budget(10000, 0, 0, 0, 20, true);
  budget.observe_fec(50, 0);
  budget.observe_fec(50, 1000);
  auto next = budget.pending(1000);
  ASSERT_TRUE(next);
  budget.request(5000);  // Arrives during driver reconfiguration.
  budget.commit(*next);
  EXPECT_EQ(budget.total(), 5000);
  EXPECT_EQ(budget.applied().encoder_kbps, 6666);  // Reinit must use the applied target.
  next = budget.pending(1001);
  ASSERT_TRUE(next);
  EXPECT_EQ(next->total_kbps, 5000);
  EXPECT_EQ(next->encoder_kbps, 3333);
  budget.commit(*next);
  budget.observe_fec(49, 2000);
  budget.observe_fec(49, 3000);
  EXPECT_FALSE(budget.pending(3000));  // One-point hysteresis.
  budget.observe_fec(0, 4000);
  budget.observe_fec(0, 5000);
  ASSERT_TRUE(budget.pending(5000));
}

TEST(VideoFec, HysteresisCannotPreventEnablingMinimumProtection) {
  streaming::bitrate_budget_t budget(10000, 0, 0, 0, 0, true);
  budget.observe_fec(1, 0);
  budget.observe_fec(1, 1000);
  const auto next = budget.pending(1000);
  ASSERT_TRUE(next);
  EXPECT_EQ(next->fec_percentage, 1);
  EXPECT_EQ(next->encoder_kbps, 9900);
}

TEST(VideoFecBudget, ArmingProtectionSkipsTheWindow) {
  streaming::bitrate_budget_t budget(10000, 0, 0, 0, 0, true);
  budget.observe_fec(50, 0);
  const auto next = budget.pending(0);
  ASSERT_TRUE(next);
  EXPECT_EQ(next->fec_percentage, 50);
  EXPECT_EQ(next->encoder_kbps, 6666);
  budget.commit(*next);
  // From an armed allowance the completed window still gates further raises.
  budget.observe_fec(50, 100);
  EXPECT_FALSE(budget.pending(999));
}

// A raise reconfigures the encoder and, on NVENC HEVC, forces a keyframe, while the per-frame
// request jitters by a few points on streams with varying frame sizes. Hold raises to a clear
// step; drops stay immediate.
TEST(VideoFecBudget, RaisesNeedAClearStepWhileDropsStayImmediate) {
  streaming::bitrate_budget_t budget(10000, 0, 0, 0, 20, true);
  budget.observe_fec(23, 0);
  budget.observe_fec(23, 1000);
  EXPECT_FALSE(budget.pending(1000));  // +3 is jitter
  budget.observe_fec(27, 2000);
  EXPECT_FALSE(budget.pending(2000));  // still the held 23
  budget.observe_fec(27, 3000);
  const auto raised = budget.pending(3000);
  ASSERT_TRUE(raised);  // +7 is a clear step
  EXPECT_EQ(raised->fec_percentage, 27);
  budget.commit(*raised);
  budget.observe_fec(25, 4000);
  EXPECT_FALSE(budget.pending(4000));  // the completed window still says 27
  budget.observe_fec(25, 5000);
  const auto dropped = budget.pending(5000);
  ASSERT_TRUE(dropped);  // the two-point drop applies one window later
  budget.commit(*dropped);
  budget.observe_fec(24, 6000);
  EXPECT_FALSE(budget.pending(6000));  // a one-point drop is still held
  // The relative floor rounds up: at 29% a raise needs six points, so 29→34 is jitter.
  streaming::bitrate_budget_t mid(10000, 0, 0, 0, 29, true);
  mid.observe_fec(34, 0);
  mid.observe_fec(34, 1000);
  EXPECT_FALSE(mid.pending(1000));
  mid.observe_fec(35, 2000);
  EXPECT_FALSE(mid.pending(2000));  // still the held 34
  mid.observe_fec(35, 3000);
  ASSERT_TRUE(mid.pending(3000));  // 29→35 clears the rounded-up bar
  // The floor grows with the ratio: at 50% a raise needs ten points.
  streaming::bitrate_budget_t high(10000, 0, 0, 0, 50, true);
  high.observe_fec(55, 0);
  high.observe_fec(55, 1000);
  EXPECT_FALSE(high.pending(1000));
  high.observe_fec(60, 2000);
  high.observe_fec(60, 3000);
  ASSERT_TRUE(high.pending(3000));
}

// While unprotected the controller must not follow the reported loss estimate down: a falling
// estimate only says the parity in place is absorbing the loss, not that the link recovered.
TEST(VideoFec, ProtectionHoldsTheStrongestLossOfItsEpochWhileLossContinues) {
  video_fec::controller_t control;
  control.initialize(20, true, 50);
  ASSERT_TRUE(control.report(1, 10000, 3000, 0));
  const auto strong = control.percentage(100, 2, 1000);
  EXPECT_GT(strong, 0);
  for (unsigned second = 2; second <= 10; ++second)
    ASSERT_TRUE(control.report(second, 10000, 30, second * 1000 - 1000));  // 0.3% after the parity landed
  ASSERT_TRUE(control.report(11, 10000, 30, 10000));
  EXPECT_EQ(control.percentage(100, 2, 10000), strong);
}

// A release that is followed by loss within seconds removed protection the link still needed:
// the next release waits twice as long without loss, and re-arms no weaker than before.
TEST(VideoFec, PrematureReleaseDemandsMoreCleanTimeBeforeTheNextRelease) {
  video_fec::controller_t control;
  control.initialize(20, true, 50);
  unsigned seq = 1;
  ASSERT_TRUE(control.report(seq++, 10000, 3000, 0));
  EXPECT_GT(control.percentage(100, 2, 1000), 0);
  for (std::int64_t t = 1000; t <= 10000; t += 1000)
    ASSERT_TRUE(control.report(seq++, 10000, 0, t));
  EXPECT_EQ(control.percentage(100, 2, 10000), 0);  // released
  ASSERT_TRUE(control.report(seq++, 10000, 2000, 12000));  // loss two seconds later
  EXPECT_GT(control.percentage(100, 2, 12000), 0);
  for (std::int64_t t = 13000; t <= 21000; t += 1000)
    ASSERT_TRUE(control.report(seq++, 10000, 0, t));
  EXPECT_GT(control.percentage(100, 2, 21000), 0);  // the fresh loss is still inside the window
  ASSERT_TRUE(control.report(seq++, 10000, 0, 22000));
  EXPECT_GT(control.percentage(100, 2, 22000), 0);  // clean read: the doubled wait starts
  for (std::int64_t t = 23000; t <= 31000; t += 1000)
    ASSERT_TRUE(control.report(seq++, 10000, 0, t));
  EXPECT_GT(control.percentage(100, 2, 31000), 0);  // nine clean seconds do not release
  ASSERT_TRUE(control.report(seq++, 10000, 0, 32000));
  EXPECT_EQ(control.percentage(100, 2, 32000), 0);  // ten do
}

// Loss that interrupts a clean stretch restarts the clean clock instead of adding to it.
TEST(VideoFec, LossDuringTheWaitRestartsTheCleanClock) {
  video_fec::controller_t control;
  control.initialize(20, true, 50);
  unsigned seq = 1;
  ASSERT_TRUE(control.report(seq++, 10000, 3000, 0));
  EXPECT_GT(control.percentage(100, 2, 1000), 0);  // arms the epoch the frame thread would query
  for (std::int64_t t = 1000; t <= 10000; t += 1000)
    ASSERT_TRUE(control.report(seq++, 10000, 0, t));
  EXPECT_EQ(control.percentage(100, 2, 10000), 0);
  ASSERT_TRUE(control.report(seq++, 10000, 2000, 12000));  // premature: the next release waits 20 s
  EXPECT_GT(control.percentage(100, 2, 12000), 0);
  for (std::int64_t t = 13000; t <= 21000; t += 1000)
    ASSERT_TRUE(control.report(seq++, 10000, 0, t));
  ASSERT_TRUE(control.report(seq++, 10000, 0, 22000));
  EXPECT_GT(control.percentage(100, 2, 22000), 0);  // clean read: the wait starts
  for (std::int64_t t = 23000; t <= 29000; t += 1000)
    ASSERT_TRUE(control.report(seq++, 10000, 0, t));
  EXPECT_GT(control.percentage(100, 2, 29000), 0);  // seven clean seconds so far
  ASSERT_TRUE(control.report(seq++, 10000, 100, 29500));  // a fresh loss resets the clock
  EXPECT_GT(control.percentage(100, 2, 29500), 0);
  for (std::int64_t t = 30500; t <= 49500; t += 1000)
    ASSERT_TRUE(control.report(seq++, 10000, 0, t));
  EXPECT_GT(control.percentage(100, 2, 49500), 0);  // the fresh clean clock just started
  ASSERT_TRUE(control.report(seq++, 10000, 0, 50500));
  EXPECT_GT(control.percentage(100, 2, 50500), 0);  // one second of clean is not enough
  for (std::int64_t t = 51500; t <= 59000; t += 1000)
    ASSERT_TRUE(control.report(seq++, 10000, 0, t));
  EXPECT_GT(control.percentage(100, 2, 59000), 0);  // 9.5 s clean since the reset
  ASSERT_TRUE(control.report(seq++, 10000, 0, 59500));
  EXPECT_EQ(control.percentage(100, 2, 59500), 0);  // 10 s clean since the reset, not since 22 s
}

// A release that stays safe halves the demanded wait again.
TEST(VideoFec, SafeReleasesHalveTheExtraWait) {
  video_fec::controller_t control;
  control.initialize(20, true, 50);
  unsigned seq = 1;
  ASSERT_TRUE(control.report(seq++, 10000, 3000, 0));
  EXPECT_GT(control.percentage(100, 2, 1000), 0);  // arms the epoch the frame thread would query
  for (std::int64_t t = 1000; t <= 10000; t += 1000)
    ASSERT_TRUE(control.report(seq++, 10000, 0, t));
  EXPECT_EQ(control.percentage(100, 2, 10000), 0);
  ASSERT_TRUE(control.report(seq++, 10000, 2000, 12000));  // premature: wait 20 s next time
  EXPECT_GT(control.percentage(100, 2, 12000), 0);  // the loss re-arms protection inside the premature window
  for (std::int64_t t = 13000; t <= 21000; t += 1000)
    ASSERT_TRUE(control.report(seq++, 10000, 0, t));
  ASSERT_TRUE(control.report(seq++, 10000, 0, 22000));
  EXPECT_GT(control.percentage(100, 2, 22000), 0);  // clean read: the doubled wait starts
  for (std::int64_t t = 23000; t <= 31000; t += 1000)
    ASSERT_TRUE(control.report(seq++, 10000, 0, t));
  EXPECT_GT(control.percentage(100, 2, 31000), 0);  // nine clean seconds do not release
  ASSERT_TRUE(control.report(seq++, 10000, 0, 32000));
  EXPECT_EQ(control.percentage(100, 2, 32000), 0);  // released after 20 clean seconds
  for (std::int64_t t = 33000; t <= 52000; t += 1000)
    ASSERT_TRUE(control.report(seq++, 10000, 0, t));
  EXPECT_EQ(control.percentage(100, 2, 52000), 0);  // still clean past the decay point: the wait halves to 5 s
  ASSERT_TRUE(control.report(seq++, 10000, 3000, 54500));  // loss much later is a fresh episode, not premature
  EXPECT_GT(control.percentage(100, 2, 54500), 0);
  for (std::int64_t t = 55500; t <= 66000; t += 1000)
    ASSERT_TRUE(control.report(seq++, 10000, 0, t));
  EXPECT_GT(control.percentage(100, 2, 66500), 0);  // the halved wait is running
  ASSERT_TRUE(control.report(seq++, 10000, 0, 67500));
  EXPECT_GT(control.percentage(100, 2, 67500), 0);  // four clean seconds
  for (std::int64_t t = 68500; t <= 70500; t += 1000)
    ASSERT_TRUE(control.report(seq++, 10000, 0, t));
  ASSERT_TRUE(control.report(seq++, 10000, 0, 71500));
  EXPECT_EQ(control.percentage(100, 2, 71500), 0);  // released after only 5 clean seconds
}

// A feedback gap must not count the unknown interval toward the clean wait: the release
// clocks restart when the history is discarded.
TEST(VideoFec, FeedbackGapRestartsTheCleanWait) {
  video_fec::controller_t control;
  control.initialize(20, true, 50);
  unsigned seq = 1;
  ASSERT_TRUE(control.report(seq++, 10000, 3000, 0));
  EXPECT_GT(control.percentage(100, 2, 1000), 0);  // arms the epoch the frame thread would query
  for (std::int64_t t = 1000; t <= 10000; t += 1000)
    ASSERT_TRUE(control.report(seq++, 10000, 0, t));
  EXPECT_EQ(control.percentage(100, 2, 10000), 0);
  ASSERT_TRUE(control.report(seq++, 10000, 2000, 12000));  // premature: wait 20 s next time
  EXPECT_GT(control.percentage(100, 2, 12000), 0);
  for (std::int64_t t = 13000; t <= 21000; t += 1000)
    ASSERT_TRUE(control.report(seq++, 10000, 0, t));
  ASSERT_TRUE(control.report(seq++, 10000, 0, 22000));
  EXPECT_GT(control.percentage(100, 2, 22000), 0);  // clean read: the wait starts
  ASSERT_TRUE(control.report(seq++, 10000, 0, 30500));  // feedback dies for 8.5 s: unknown, not clean
  EXPECT_EQ(control.percentage(100, 2, 30500), 20);  // the resume window holds the fallback
  for (std::int64_t t = 31500; t <= 40500; t += 1000)
    ASSERT_TRUE(control.report(seq++, 10000, 0, t));
  EXPECT_GT(control.percentage(100, 2, 40500), 0);  // the wait restarts, not credited from 22 s
  for (std::int64_t t = 41500; t <= 49500; t += 1000)
    ASSERT_TRUE(control.report(seq++, 10000, 0, t));
  EXPECT_GT(control.percentage(100, 2, 49500), 0);  // nine clean seconds since the restart
  ASSERT_TRUE(control.report(seq++, 10000, 0, 50500));
  EXPECT_EQ(control.percentage(100, 2, 50500), 0);  // released ten seconds after the restart
}

// A host whose default ratio is zero never holds protection, so its first loss must not pay
// the premature-release penalty for a "release" of nothing.
TEST(VideoFec, StartupZeroIsNotARelease) {
  video_fec::controller_t control;
  control.initialize(0, true, 50);
  ASSERT_TRUE(control.report(1, 10000, 0, 0));
  EXPECT_EQ(control.percentage(100, 2, 0), 0);
  ASSERT_TRUE(control.report(2, 10000, 2000, 1000));  // the first loss
  EXPECT_GT(control.percentage(100, 2, 1000), 0);  // arms without any penalty
  for (std::int64_t t = 2000; t <= 10000; t += 1000) {
    ASSERT_TRUE(control.report(static_cast<unsigned>(t / 1000 + 1), 10000, 0, t));
    if (t == 5000) EXPECT_GT(control.percentage(100, 2, t), 0);  // still protected mid-window
  }
  ASSERT_TRUE(control.report(12, 10000, 0, 11000));
  EXPECT_EQ(control.percentage(100, 2, 11000), 0);  // released after the plain window, not 20 s
}

// ---- the host's bitrate ceiling is live (tray 码率上限): a running session's budget follows it ----

TEST(HostBitrateCeiling, TextMeansAWholeNumberOfKbpsAndZeroForAnythingElse) {
  EXPECT_EQ(streaming::parse_bitrate_ceiling_kbps("50000"), 50000);
  EXPECT_EQ(streaming::parse_bitrate_ceiling_kbps("0"), 0);
  for (const auto text : { "", "auto", "-5", "12.5", " 7", "7 ", "1e3", "99999999999999999999" })
    EXPECT_EQ(streaming::parse_bitrate_ceiling_kbps(text), 0) << text;
}

TEST(HostBitrateCeiling, ALoweredCeilingBringsARunningSessionDownAtOnce) {
  streaming::bitrate_budget_t budget(80000, 0, 0, 0, 0, false);  // no ceiling when it was made
  ASSERT_EQ(budget.total(), 80000);
  EXPECT_EQ(budget.set_maximum(30000), 30000);
  EXPECT_EQ(budget.total(), 30000);
  // the encoding thread sees it through pending() like any other request
  const auto next = budget.pending(1000);
  ASSERT_TRUE(next);
  EXPECT_EQ(next->total_kbps, 30000);
  EXPECT_EQ(next->encoder_kbps, 30000);
  budget.commit(*next);
  EXPECT_EQ(budget.applied().total_kbps, 30000);
}

TEST(HostBitrateCeiling, ASessionBelowTheNewCeilingIsLeftAlone) {
  streaming::bitrate_budget_t budget(20000, 0, 0, 0, 0, false);
  EXPECT_EQ(budget.set_maximum(30000), 20000);
  EXPECT_FALSE(budget.pending(1000));
}

TEST(HostBitrateCeiling, ARaisedOrRemovedCeilingGivesBackWhatTheClientLastAskedFor) {
  streaming::bitrate_budget_t budget(80000, 40000, 0, 0, 0, false);  // made under a 40 Mbps ceiling, the client wanted 80
  ASSERT_EQ(budget.total(), 40000);
  EXPECT_EQ(budget.set_maximum(60000), 60000);  // min(asked 80000, new ceiling 60000)
  EXPECT_EQ(budget.set_maximum(100000), 80000);  // above what was asked: what was asked
  EXPECT_EQ(budget.set_maximum(0), 80000);  // 0 = automatic, no ceiling
  EXPECT_EQ(budget.maximum(), 0);
}

TEST(HostBitrateCeiling, WhatTheAbrAsksForIsHeldToTheCeilingAndRemembered) {
  streaming::bitrate_budget_t budget(50000, 0, 0, 0, 0, false);
  budget.set_maximum(30000);
  EXPECT_EQ(budget.request(45000), 30000);  // the ABR wants up: the ceiling holds
  EXPECT_EQ(budget.request(20000), 20000);  // and down: it follows
  EXPECT_EQ(budget.set_maximum(25000), 20000);  // a ceiling above the session changes nothing
  EXPECT_EQ(budget.set_maximum(0), 20000);  // what the ABR last asked, not the first request
}

TEST(HostBitrateCeiling, EverySessionFollowsTheSameCeilingEachToItsOwnRequest) {
  std::vector<std::shared_ptr<streaming::bitrate_budget_t>> sessions;
  for (const int asked : { 12000, 45000, 90000 }) sessions.push_back(std::make_shared<streaming::bitrate_budget_t>(asked, 0, 0, 0, 0, false));
  const auto totals = [&] { std::vector<int> t; for (const auto &s : sessions) t.push_back(s->total()); return t; };
  EXPECT_EQ(totals(), (std::vector<int> { 12000, 45000, 90000 }));
  for (const auto &s : sessions) s->set_maximum(30000);
  EXPECT_EQ(totals(), (std::vector<int> { 12000, 30000, 30000 }));
  for (const auto &s : sessions) s->set_maximum(60000);
  EXPECT_EQ(totals(), (std::vector<int> { 12000, 45000, 60000 }));
  for (const auto &s : sessions) s->set_maximum(0);
  EXPECT_EQ(totals(), (std::vector<int> { 12000, 45000, 90000 }));
}

TEST(HostBitrateCeiling, TheFecAllowanceAndTheReservesStillComeOutOfTheNewTotal) {
  streaming::bitrate_budget_t budget(60000, 0, 512, 500, 20, false);
  const auto before = budget.applied();
  budget.set_maximum(30000);
  const auto next = budget.pending(1000);
  ASSERT_TRUE(next);
  EXPECT_EQ(next->total_kbps, 30000);
  EXPECT_LT(next->encoder_kbps, 30000);  // repair, audio and overhead are inside the total
  EXPECT_LT(next->encoder_kbps, before.encoder_kbps);
  EXPECT_EQ(next->fec_percentage, 20U);
}
