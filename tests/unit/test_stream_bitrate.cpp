/**
 * @file tests/unit/test_stream_bitrate.cpp
 * @brief Tests for stream bitrate budget accounting.
 */

#include <gtest/gtest.h>

#include "src/stream_bitrate.h"

TEST(StreamBitrateTests, FiveMbpsHighSurroundKeepsUsableVideoBudget) {
  EXPECT_GE(stream_bitrate::encoding_bitrate_from_configured_total_kbps(5000, 35, true, 12), 3000);
}

TEST(StreamBitrateTests, FecOverheadIsReservedOnceWithoutChannelCrushing) {
  EXPECT_EQ(stream_bitrate::encoding_bitrate_from_configured_total_kbps(5000, 35, false, 2), 3250);
  EXPECT_EQ(stream_bitrate::encoding_bitrate_from_configured_total_kbps(10000, 35, true, 12), 6500);
}
