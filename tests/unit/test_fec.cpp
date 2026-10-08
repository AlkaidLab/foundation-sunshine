#include "src/fec.h"
#include <gtest/gtest.h>

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
