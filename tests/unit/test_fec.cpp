#include "src/streaming/bitrate.h"
#include "src/streaming/fec.h"
#include <gtest/gtest.h>
#include <limits>

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
      if (total >= 4) EXPECT_LE(static_cast<std::int64_t>(encoder) * (100 + ratio), static_cast<std::int64_t>(total) * 100);
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

TEST(VideoFecBudget, HysteresisCannotPreventEnablingMinimumProtection) {
  streaming::bitrate_budget_t budget(10000, 0, 0, 0, 0, true);
  budget.observe_fec(1, 0);
  budget.observe_fec(1, 1000);
  const auto next = budget.pending(1000);
  ASSERT_TRUE(next);
  EXPECT_EQ(next->fec_percentage, 1);
  EXPECT_EQ(next->encoder_kbps, 9900);
}
