/**
 * @file tests/unit/test_video_bitrate.cpp
 * @brief Tests for dynamic encoder bitrate accounting.
 */
#include <gtest/gtest.h>

#include "src/video_bitrate.h"

TEST(VideoBitrateTests, DynamicEncoderBitrateIsAlreadyNetOfFecOverhead) {
  EXPECT_EQ(video::dynamic_encoder_bitrate_kbps(2340, 35), 2340);
  EXPECT_EQ(video::dynamic_encoder_bitrate_kbps(2340, 80), 2340);
  EXPECT_EQ(video::dynamic_encoder_bitrate_kbps(1500, 35), 1500);
}

TEST(VideoBitrateTests, DynamicEncoderBitrateRejectsInvalidTargets) {
  EXPECT_EQ(video::dynamic_encoder_bitrate_kbps(0, 35), 0);
  EXPECT_EQ(video::dynamic_encoder_bitrate_kbps(-100, 35), -100);
}
