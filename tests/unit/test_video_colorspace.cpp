/**
 * @file tests/unit/test_video_colorspace.cpp
 * @brief Test src/video_colorspace.*.
 */
#include <src/video_colorspace.h>
#include <src/video.h>

#include "../tests_common.h"

using namespace video;

class ColorspaceTest: public ::testing::Test {
protected:
  config_t config;

  void SetUp() override {
    // Initialize with default values
    config.width = 1920;
    config.height = 1080;
    config.framerate = 60;
    config.bitrate = 20000;
    config.slicesPerFrame = 1;
    config.numRefFrames = 0;
    config.encoderCscMode = 0;  // Limited range, Rec. 601
    config.videoFormat = 0;  // H.264
    config.dynamicRange = 0;  // SDR 8-bit
    config.chromaSamplingType = 0;  // 4:2:0
    config.enableIntraRefresh = 0;
  }
};

TEST_F(ColorspaceTest, DefaultValues) {
  // Test that sunshine_colorspace_t has proper default values
  sunshine_colorspace_t colorspace;

  EXPECT_EQ(colorspace.colorspace, colorspace_e::rec709);
  EXPECT_EQ(colorspace.full_range, false);
  EXPECT_EQ(colorspace.bit_depth, 8);
}

TEST_F(ColorspaceTest, SDR_Rec601_LimitedRange) {
  config.encoderCscMode = 0;  // Limited range, Rec. 601
  config.dynamicRange = 0;  // SDR 8-bit

  auto colorspace = colorspace_from_client_config(config, false);

  EXPECT_EQ(colorspace.colorspace, colorspace_e::rec601);
  EXPECT_EQ(colorspace.full_range, false);
  EXPECT_EQ(colorspace.bit_depth, 8);
}

TEST_F(ColorspaceTest, SDR_Rec601_FullRange) {
  config.encoderCscMode = 1;  // Full range, Rec. 601
  config.dynamicRange = 0;  // SDR 8-bit

  auto colorspace = colorspace_from_client_config(config, false);

  EXPECT_EQ(colorspace.colorspace, colorspace_e::rec601);
  EXPECT_EQ(colorspace.full_range, true);
  EXPECT_EQ(colorspace.bit_depth, 8);
}

TEST_F(ColorspaceTest, SDR_Rec709_LimitedRange) {
  config.encoderCscMode = 2;  // Limited range, Rec. 709
  config.dynamicRange = 0;  // SDR 8-bit

  auto colorspace = colorspace_from_client_config(config, false);

  EXPECT_EQ(colorspace.colorspace, colorspace_e::rec709);
  EXPECT_EQ(colorspace.full_range, false);
  EXPECT_EQ(colorspace.bit_depth, 8);
}

TEST_F(ColorspaceTest, SDR_Rec709_FullRange) {
  config.encoderCscMode = 3;  // Full range, Rec. 709
  config.dynamicRange = 0;  // SDR 8-bit

  auto colorspace = colorspace_from_client_config(config, false);

  EXPECT_EQ(colorspace.colorspace, colorspace_e::rec709);
  EXPECT_EQ(colorspace.full_range, true);
  EXPECT_EQ(colorspace.bit_depth, 8);
}

TEST_F(ColorspaceTest, SDR_BT2020_LimitedRange) {
  config.encoderCscMode = 4;  // Limited range, BT.2020
  config.dynamicRange = 0;  // SDR 8-bit (invalid for BT.2020 SDR)

  auto colorspace = colorspace_from_client_config(config, false);

  // BT.2020 SDR requires 10-bit, should fallback to Rec. 709
  EXPECT_EQ(colorspace.colorspace, colorspace_e::rec709);
  EXPECT_EQ(colorspace.full_range, false);
  EXPECT_EQ(colorspace.bit_depth, 8);
}

TEST_F(ColorspaceTest, HDR_PQ_NoHDRDisplay) {
  config.encoderCscMode = 0;  // Limited range
  config.dynamicRange = 1;  // HDR 10-bit with PQ

  // HDR requested but display doesn't support HDR
  auto colorspace = colorspace_from_client_config(config, false);

  // Should fallback to SDR
  EXPECT_EQ(colorspace.colorspace, colorspace_e::rec601);
  EXPECT_EQ(colorspace.full_range, false);
  EXPECT_EQ(colorspace.bit_depth, 10);  // Still 10-bit
}

TEST_F(ColorspaceTest, HDR_PQ_WithHDRDisplay) {
  config.encoderCscMode = 0;  // Limited range
  config.dynamicRange = 1;  // HDR 10-bit with PQ

  // HDR requested and display supports HDR
  auto colorspace = colorspace_from_client_config(config, true);

  EXPECT_EQ(colorspace.colorspace, colorspace_e::bt2020);
  EXPECT_EQ(colorspace.full_range, false);
  EXPECT_EQ(colorspace.bit_depth, 10);
}

TEST_F(ColorspaceTest, HDR_HLG_WithHDRDisplay) {
  config.encoderCscMode = 0;  // Limited range
  config.dynamicRange = 2;  // HDR 10-bit with HLG

  // HDR requested and display supports HDR
  auto colorspace = colorspace_from_client_config(config, true);

  EXPECT_EQ(colorspace.colorspace, colorspace_e::bt2020hlg);
  EXPECT_EQ(colorspace.full_range, false);
  EXPECT_EQ(colorspace.bit_depth, 10);
}

TEST_F(ColorspaceTest, InvalidDynamicRange) {
  config.encoderCscMode = 0;  // Limited range
  config.dynamicRange = 99;  // Invalid value

  auto colorspace = colorspace_from_client_config(config, false);

  // Should fallback to 10-bit
  EXPECT_EQ(colorspace.bit_depth, 10);
}

TEST_F(ColorspaceTest, InvalidEncoderCscMode) {
  config.encoderCscMode = 99;  // Invalid value
  config.dynamicRange = 0;  // SDR 8-bit

  auto colorspace = colorspace_from_client_config(config, false);

  // Should fallback to Rec. 709
  EXPECT_EQ(colorspace.colorspace, colorspace_e::rec709);
  EXPECT_EQ(colorspace.bit_depth, 8);
}

TEST_F(ColorspaceTest, ColorspaceIsHDR) {
  sunshine_colorspace_t sdr_colorspace;
  sdr_colorspace.colorspace = colorspace_e::rec709;
  EXPECT_FALSE(colorspace_is_hdr(sdr_colorspace));

  sunshine_colorspace_t hdr_pq_colorspace;
  hdr_pq_colorspace.colorspace = colorspace_e::bt2020;
  EXPECT_TRUE(colorspace_is_hdr(hdr_pq_colorspace));

  sunshine_colorspace_t hdr_hlg_colorspace;
  hdr_hlg_colorspace.colorspace = colorspace_e::bt2020hlg;
  EXPECT_TRUE(colorspace_is_hdr(hdr_hlg_colorspace));
}

TEST_F(ColorspaceTest, ColorspaceIsHLG) {
  sunshine_colorspace_t hdr_pq_colorspace;
  hdr_pq_colorspace.colorspace = colorspace_e::bt2020;
  EXPECT_FALSE(colorspace_is_hlg(hdr_pq_colorspace));

  sunshine_colorspace_t hdr_hlg_colorspace;
  hdr_hlg_colorspace.colorspace = colorspace_e::bt2020hlg;
  EXPECT_TRUE(colorspace_is_hlg(hdr_hlg_colorspace));
}

TEST_F(ColorspaceTest, ColorspaceIsPQ) {
  sunshine_colorspace_t hdr_pq_colorspace;
  hdr_pq_colorspace.colorspace = colorspace_e::bt2020;
  EXPECT_TRUE(colorspace_is_pq(hdr_pq_colorspace));

  sunshine_colorspace_t hdr_hlg_colorspace;
  hdr_hlg_colorspace.colorspace = colorspace_e::bt2020hlg;
  EXPECT_FALSE(colorspace_is_pq(hdr_hlg_colorspace));
}

TEST_F(ColorspaceTest, AVCodecColorspaceConversion) {
  sunshine_colorspace_t colorspace;
  colorspace.colorspace = colorspace_e::rec709;
  colorspace.full_range = false;
  colorspace.bit_depth = 8;

  auto avcodec_colorspace = avcodec_colorspace_from_sunshine_colorspace(colorspace);

  EXPECT_EQ(avcodec_colorspace.primaries, AVCOL_PRI_BT709);
  EXPECT_EQ(avcodec_colorspace.transfer_function, AVCOL_TRC_BT709);
  EXPECT_EQ(avcodec_colorspace.matrix, AVCOL_SPC_BT709);
  EXPECT_EQ(avcodec_colorspace.range, AVCOL_RANGE_MPEG);
}

TEST_F(ColorspaceTest, ColorVectorsFromColorspace) {
  sunshine_colorspace_t colorspace;
  colorspace.colorspace = colorspace_e::rec709;
  colorspace.full_range = false;
  colorspace.bit_depth = 8;

  auto color_vectors = color_vectors_from_colorspace(colorspace, false);

  EXPECT_NE(color_vectors, nullptr);
  // Verify that color vectors are properly initialized
  EXPECT_GT(color_vectors->color_vec_y[0], 0.0f);
  EXPECT_GT(color_vectors->color_vec_y[1], 0.0f);
  EXPECT_GT(color_vectors->color_vec_y[2], 0.0f);
}

