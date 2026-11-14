#include "src/streaming/video_pacing.h"

#include <gtest/gtest.h>

TEST(VideoPacing, UsesMeasuredRateAndHeadroom) {
  EXPECT_EQ(streaming::video_pacing_rate_bps(0, 10'000), 8'000'000u);
  EXPECT_EQ(streaming::video_pacing_rate_bps(20'000, 10'000), 20'000'000u);
  EXPECT_EQ(streaming::video_pacing_rate_bps(5'000, 10'000), 8'000'000u);
  EXPECT_EQ(streaming::video_pacing_rate_bps(0, 0), 0u);
}

TEST(VideoPacing, OnlyLargeFramesArePaced) {
  EXPECT_FALSE(streaming::should_pace_video_frame(false, 65'535, 65'536));
  EXPECT_TRUE(streaming::should_pace_video_frame(false, 65'536, 65'536));
  EXPECT_TRUE(streaming::should_pace_video_frame(true, 65'536, 65'536));
  EXPECT_FALSE(streaming::should_pace_video_frame(true, 10, 65'536));
  EXPECT_FALSE(streaming::should_pace_video_frame(true, 100, 0));
}

TEST(VideoPacing, SlotsAccumulateAtConfiguredRate) {
  constexpr auto rate = 8'000'000u;
  constexpr auto packet_bytes = 1'000u;
  EXPECT_EQ(streaming::video_packet_slot(0, packet_bytes, rate).count(), 0);
  EXPECT_NEAR(streaming::video_packet_slot(1, packet_bytes, rate).count(), 1'000'000, 1);
  EXPECT_NEAR(streaming::video_packet_slot(8, packet_bytes, rate).count(), 8'000'000, 1);
  EXPECT_EQ(streaming::video_packet_slot(1, packet_bytes, 0).count(), 0);
}
