/**
 * @file tests/unit/test_video.cpp
 * @brief Test src/video.*.
 */
#include <src/video.h>

#include <src/config.h>
#include <src/display_device/display_device.h>

#include "../tests_common.h"

#include <algorithm>
#include <limits>

namespace {

  double
  millis(std::chrono::duration<double, std::milli> duration) {
    return duration.count();
  }

}  // namespace

TEST(VideoInputActivityBoostPolicy, UsesConfiguredBoostCadenceWhenMinimumIsAuto) {
  const auto base_frame_time = video::minimum_frame_time_for_vrr(240, 0);
  const auto policy = video::make_input_activity_boost_policy({
    true,
    true,
    240,
    0,
    60,
    150,
  });

  ASSERT_TRUE(policy.configured);
  ASSERT_TRUE(policy.useful);
  EXPECT_EQ(policy.fps, 60);
  EXPECT_NEAR(millis(base_frame_time), 1000.0 / 120.0, 0.001);
  EXPECT_NEAR(millis(policy.frame_time), 1000.0 / 60.0, 0.001);

  const auto effective_frame_time = video::effective_minimum_frame_time(base_frame_time, policy, true, 0);
  EXPECT_NEAR(millis(effective_frame_time), 1000.0 / 60.0, 0.001);
}

TEST(VideoInputActivityBoostPolicy, KeepsAutoBoostAtConfiguredCadenceOn144FpsStreams) {
  const auto base_frame_time = video::minimum_frame_time_for_vrr(144, 0);
  const auto policy = video::make_input_activity_boost_policy({
    true,
    true,
    144,
    0,
    60,
    150,
  });

  ASSERT_TRUE(policy.useful);
  EXPECT_NEAR(millis(base_frame_time), 1000.0 / 72.0, 0.001);
  EXPECT_NEAR(millis(video::effective_minimum_frame_time(base_frame_time, policy, true, 0)), 1000.0 / 60.0, 0.001);
}

TEST(VideoInputActivityBoostPolicy, DisablesBoostWhenExplicitMinimumIsAlreadyAsFast) {
  const auto policy = video::make_input_activity_boost_policy({
    true,
    true,
    144,
    60,
    60,
    150,
  });

  EXPECT_TRUE(policy.configured);
  EXPECT_FALSE(policy.useful);

  const auto base_frame_time = video::minimum_frame_time_for_vrr(144, 60);
  EXPECT_NEAR(millis(video::effective_minimum_frame_time(base_frame_time, policy, true, 60)), 1000.0 / 60.0, 0.001);
}

TEST(VideoInputActivityBoostPolicy, UsesBoostWhenExplicitMinimumIsSlower) {
  const auto base_frame_time = video::minimum_frame_time_for_vrr(144, 30);
  const auto policy = video::make_input_activity_boost_policy({
    true,
    true,
    144,
    30,
    60,
    150,
  });

  ASSERT_TRUE(policy.useful);
  EXPECT_NEAR(millis(video::effective_minimum_frame_time(base_frame_time, policy, true, 30)), 1000.0 / 60.0, 0.001);
}

TEST(VideoInputActivityBoostPolicy, CapsBoostAtStreamFps) {
  const auto policy = video::make_input_activity_boost_policy({
    true,
    true,
    120,
    0,
    240,
    150,
  });

  ASSERT_TRUE(policy.useful);
  EXPECT_EQ(policy.fps, 120);
  EXPECT_NEAR(millis(policy.frame_time), 1000.0 / 120.0, 0.001);
}

TEST(HlgSystemGamma, UsesBt2100ProductionMonitorFormula) {
  EXPECT_NEAR(video::hlg_system_gamma(400.0f), 1.0328653f, 0.00001f);
  EXPECT_NEAR(video::hlg_system_gamma(678.0f), 1.12912f, 0.00001f);
  EXPECT_NEAR(video::hlg_system_gamma(1000.0f), 1.2f, 0.00001f);
  EXPECT_NEAR(video::hlg_system_gamma(2000.0f), 1.32643f, 0.00001f);
}

TEST(HlgSystemGamma, UsesBt2100ExtendedRangeFormula) {
  EXPECT_NEAR(video::hlg_system_gamma(300.0f), 0.99949f, 0.00001f);
  EXPECT_NEAR(video::hlg_system_gamma(4000.0f), 1.48119f, 0.00001f);
}

TEST(HlgSystemGamma, FallsBackToReferencePeakForInvalidValues) {
  EXPECT_NEAR(video::hlg_system_gamma(0.0f), 1.2f, 0.00001f);
  EXPECT_NEAR(video::hlg_system_gamma(-1.0f), 1.2f, 0.00001f);
  EXPECT_NEAR(video::hlg_system_gamma(std::numeric_limits<float>::quiet_NaN()), 1.2f, 0.00001f);
}

TEST(DynamicSdrWhite, AcceptsOnlyFiniteValuesWithinProtocolRange) {
  EXPECT_FALSE(video::is_valid_client_sdr_white_nits(49.0f));
  EXPECT_TRUE(video::is_valid_client_sdr_white_nits(50.0f));
  EXPECT_TRUE(video::is_valid_client_sdr_white_nits(1000.0f));
  EXPECT_FALSE(video::is_valid_client_sdr_white_nits(1001.0f));
  EXPECT_FALSE(video::is_valid_client_sdr_white_nits(std::numeric_limits<float>::quiet_NaN()));
  EXPECT_FALSE(video::is_valid_client_sdr_white_nits(std::numeric_limits<float>::infinity()));
}

TEST(VideoBitrate, ConvertsTotalBitrateToEncoderBitrate) {
  EXPECT_EQ(video::encoder_bitrate_from_total_bitrate(50000, 10), 45000);
  EXPECT_EQ(video::encoder_bitrate_from_total_bitrate(50000, 80), 10000);
  EXPECT_EQ(video::encoder_bitrate_from_total_bitrate(50000, 0), 50000);
  EXPECT_EQ(video::encoder_bitrate_from_total_bitrate(50000, -1), 50000);
  EXPECT_EQ(video::encoder_bitrate_from_total_bitrate(50000, 81), 50000);
}

TEST(VideoBitrate, ConvertsCappedTotalRequestToEncoderBitrate) {
  EXPECT_EQ(video::encoder_bitrate_for_total_request(90000, 0, 10), 81000);
  EXPECT_EQ(video::encoder_bitrate_for_total_request(90000, 50000, 10), 45000);
  EXPECT_EQ(video::encoder_bitrate_for_total_request(40000, 50000, 10), 36000);
}

TEST(VideoBitrate, CapsInitialEncoderBitrateUsingTotalBitrateLimit) {
  EXPECT_EQ(video::cap_initial_encoder_bitrate(90000, 0, 10), 90000);
  EXPECT_EQ(video::cap_initial_encoder_bitrate(90000, 50000, 10), 45000);
  EXPECT_EQ(video::cap_initial_encoder_bitrate(40000, 50000, 10), 40000);
}

TEST(HdrPipelineStatus, RegistersUpdatesAndRemovesPipelineState) {
  video::hdr_pipeline_status_t status {
    .hdr_mode = "hlg",
    .analysis_mode = "auto",
    .analysis_active = true,
    .metadata_formats = { "hdr_vivid" },
    .conversion_path = "compute_shader_direct",
  };

  const auto id = video::register_hdr_pipeline_status(status);
  auto statuses = video::get_hdr_pipeline_statuses();
  auto entry = std::ranges::find(statuses, id, &video::hdr_pipeline_status_t::id);
  ASSERT_NE(entry, statuses.end());
  EXPECT_EQ(entry->hdr_mode, "hlg");
  EXPECT_FALSE(entry->scene_metadata_active);

  status.scene_metadata_active = true;
  video::update_hdr_pipeline_status(id, status);
  statuses = video::get_hdr_pipeline_statuses();
  entry = std::ranges::find(statuses, id, &video::hdr_pipeline_status_t::id);
  ASSERT_NE(entry, statuses.end());
  EXPECT_TRUE(entry->scene_metadata_active);

  video::unregister_hdr_pipeline_status(id);
  statuses = video::get_hdr_pipeline_statuses();
  EXPECT_EQ(std::ranges::find(statuses, id, &video::hdr_pipeline_status_t::id), statuses.end());
}

struct EncoderTest: PlatformTestSuite, testing::WithParamInterface<video::encoder_t *> {
  void
  SetUp() override {
    auto &encoder = *GetParam();
    const auto probe_display_name = display_device::get_display_name(config::video.output_name);
    if (!video::validate_encoder(encoder, false, std::nullopt, probe_display_name)) {
      // Encoder failed validation,
      // if it's software - fail, otherwise skip
      if (encoder.name == "software") {
        FAIL() << "Software encoder not available";
      }
      else {
        GTEST_SKIP() << "Encoder not available";
      }
    }
  }
};

INSTANTIATE_TEST_SUITE_P(
  EncoderVariants,
  EncoderTest,
  testing::Values(
#if !defined(__APPLE__)
    &video::nvenc,
#endif
#ifdef _WIN32
    &video::amdvce,
    &video::quicksync,
#endif
#ifdef __linux__
    &video::vaapi,
#endif
#ifdef __APPLE__
    &video::videotoolbox,
#endif
    &video::software),
  [](const auto &info) { return std::string(info.param->name); });

TEST_P(EncoderTest, ValidateEncoder) {
  // todo:: test something besides fixture setup
}
