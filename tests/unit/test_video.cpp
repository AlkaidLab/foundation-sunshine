/**
 * @file tests/unit/test_video.cpp
 * @brief Test src/video.*.
 */
#include <src/video.h>

#include "../tests_common.h"

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

struct EncoderTest: PlatformTestSuite, testing::WithParamInterface<video::encoder_t *> {
  void
  SetUp() override {
    auto &encoder = *GetParam();
    if (!video::validate_encoder(encoder, false)) {
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
