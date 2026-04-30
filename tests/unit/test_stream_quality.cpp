/**
 * @file tests/unit/test_stream_quality.cpp
 * @brief Test low-bitrate screen clarity planning.
 */

#include "src/stream_quality.h"

#include <gtest/gtest.h>

TEST(StreamQualityTests, KeepsHighBudgetStreamsUnchanged) {
  auto plan = stream_quality::plan_low_bitrate_clarity({
    .width = 1920,
    .height = 1080,
    .fps = 60,
    .video_bitrate_kbps = 20000,
    .video_format = 1,
    .chroma_sampling_type = 1,
  });

  EXPECT_FALSE(plan.enabled);
  EXPECT_EQ(plan.effective_fps, 60);
  EXPECT_EQ(plan.effective_chroma_sampling_type, 1);
}

TEST(StreamQualityTests, PrefersLumaDetailOverYuv444WhenPixelBudgetIsStarved) {
  auto plan = stream_quality::plan_low_bitrate_clarity({
    .width = 3024,
    .height = 1900,
    .fps = 120,
    .video_bitrate_kbps = 5000,
    .video_format = 1,
    .chroma_sampling_type = 1,
  });

  EXPECT_TRUE(plan.enabled);
  EXPECT_EQ(plan.effective_chroma_sampling_type, 0);
  EXPECT_LT(plan.effective_fps, 120);
  EXPECT_GE(plan.effective_fps, 45);
}

TEST(StreamQualityTests, DoesNotRaiseBitrateToImproveLowBitrateClarity) {
  auto plan = stream_quality::plan_low_bitrate_clarity({
    .width = 3024,
    .height = 1900,
    .fps = 120,
    .video_bitrate_kbps = 2000,
    .video_format = 1,
    .chroma_sampling_type = 0,
  });

  EXPECT_TRUE(plan.enabled);
  EXPECT_EQ(plan.video_bitrate_kbps, 2000);
  EXPECT_GE(plan.effective_fps, 24);
  EXPECT_LE(plan.effective_fps, 35);
}

TEST(StreamQualityTests, TextDesktopEnablesRoiAndSharperLumaAtLowBitrate) {
  auto plan = stream_quality::plan_low_bitrate_clarity({
    .width = 2560,
    .height = 1440,
    .fps = 60,
    .video_bitrate_kbps = 2500,
    .video_format = 1,
    .chroma_sampling_type = 1,
    .content_type = stream_quality::content_type_e::text,
  });

  EXPECT_TRUE(plan.enabled);
  EXPECT_EQ(plan.effective_chroma_sampling_type, 0);
  EXPECT_TRUE(plan.roi_enabled);
  EXPECT_TRUE(plan.dirty_region_priority);
  EXPECT_TRUE(plan.prefer_long_term_reference);
  EXPECT_GE(plan.sharpen_alpha, 0.15f);
  EXPECT_GE(plan.target_qp, 20);
}

TEST(StreamQualityTests, MotionKeepsHigherFpsThanTextAtSameBudget) {
  auto text_plan = stream_quality::plan_low_bitrate_clarity({
    .width = 1920,
    .height = 1080,
    .fps = 60,
    .video_bitrate_kbps = 2200,
    .video_format = 1,
    .chroma_sampling_type = 0,
    .content_type = stream_quality::content_type_e::text,
  });

  auto motion_plan = stream_quality::plan_low_bitrate_clarity({
    .width = 1920,
    .height = 1080,
    .fps = 60,
    .video_bitrate_kbps = 2200,
    .video_format = 1,
    .chroma_sampling_type = 0,
    .content_type = stream_quality::content_type_e::motion,
  });

  EXPECT_TRUE(text_plan.enabled);
  EXPECT_TRUE(motion_plan.enabled);
  EXPECT_GE(motion_plan.effective_fps, text_plan.effective_fps);
  EXPECT_TRUE(motion_plan.prefer_intra_refresh);
}

TEST(StreamQualityTests, HighCeilingStreamsUseSafeStartupBudget) {
  auto startup_bitrate = stream_quality::startup_bitrate_for_ceiling({
    .width = 3024,
    .height = 1900,
    .fps = 120,
    .video_bitrate_kbps = 120000,
    .video_format = 1,
    .chroma_sampling_type = 1,
  });

  EXPECT_GE(startup_bitrate, 12000);
  EXPECT_LE(startup_bitrate, 30000);
  EXPECT_LT(startup_bitrate, 120000);
}

TEST(StreamQualityTests, ModerateCeilingStreamsStartAtRequestedBudget) {
  auto startup_bitrate = stream_quality::startup_bitrate_for_ceiling({
    .width = 2560,
    .height = 1440,
    .fps = 60,
    .video_bitrate_kbps = 20000,
    .video_format = 1,
    .chroma_sampling_type = 0,
  });

  EXPECT_EQ(startup_bitrate, 20000);
}

TEST(StreamQualityTests, HighPixelRateEnhancedStreamsRampEvenAtModerateCeiling) {
  auto startup_bitrate = stream_quality::startup_bitrate_for_ceiling({
    .width = 3024,
    .height = 1900,
    .fps = 120,
    .video_bitrate_kbps = 18000,
    .video_format = 1,
    .chroma_sampling_type = 0,
  });

  EXPECT_GE(startup_bitrate, 9000);
  EXPECT_LT(startup_bitrate, 18000);

  auto startup_fps = stream_quality::startup_fps_for_bitrate({
    .width = 3024,
    .height = 1900,
    .fps = 120,
    .video_bitrate_kbps = 18000,
    .video_format = 1,
    .chroma_sampling_type = 0,
  }, startup_bitrate);

  EXPECT_GE(startup_fps, 60);
  EXPECT_LT(startup_fps, 120);
}

TEST(StreamQualityTests, MotionLowBudgetTradesExcessFpsForClarity) {
  auto plan = stream_quality::plan_low_bitrate_clarity({
    .width = 3024,
    .height = 1900,
    .fps = 120,
    .video_bitrate_kbps = 5000,
    .video_format = 1,
    .chroma_sampling_type = 0,
    .content_type = stream_quality::content_type_e::motion,
  });

  EXPECT_TRUE(plan.enabled);
  EXPECT_GE(plan.effective_fps, 72);
  EXPECT_LE(plan.effective_fps, 90);
  EXPECT_TRUE(plan.prefer_intra_refresh);
}

TEST(StreamQualityTests, HighRefreshGameKeepsInteractiveFpsFloorBeforeBecomingPpt) {
  auto plan = stream_quality::plan_low_bitrate_clarity({
    .width = 3024,
    .height = 1900,
    .fps = 120,
    .video_bitrate_kbps = 5000,
    .video_format = 1,
    .chroma_sampling_type = 1,
    .content_type = stream_quality::content_type_e::game,
  });

  EXPECT_TRUE(plan.enabled);
  EXPECT_EQ(plan.effective_chroma_sampling_type, 0);
  EXPECT_GE(plan.effective_fps, 72);
  EXPECT_TRUE(plan.prefer_temporal_layers);
  EXPECT_TRUE(plan.discardable_enhancement_layer);
}

TEST(StreamQualityTests, HighRefreshInteractiveDesktopKeepsSixtyFpsFloor) {
  auto plan = stream_quality::plan_low_bitrate_clarity({
    .width = 3024,
    .height = 1900,
    .fps = 120,
    .video_bitrate_kbps = 2340,
    .video_format = 1,
    .chroma_sampling_type = 0,
    .content_type = stream_quality::content_type_e::desktop,
  });

  EXPECT_TRUE(plan.enabled);
  EXPECT_GE(plan.effective_fps, 60);
  EXPECT_TRUE(plan.prefer_temporal_layers);
  EXPECT_TRUE(plan.discardable_enhancement_layer);
}

TEST(StreamQualityTests, HighRefreshGameKeepsSixtyFpsFloorAtLowBudget) {
  auto plan = stream_quality::plan_low_bitrate_clarity({
    .width = 3024,
    .height = 1900,
    .fps = 120,
    .video_bitrate_kbps = 2340,
    .video_format = 1,
    .chroma_sampling_type = 0,
    .content_type = stream_quality::content_type_e::game,
  });

  EXPECT_TRUE(plan.enabled);
  EXPECT_GE(plan.effective_fps, 60);
  EXPECT_TRUE(plan.prefer_temporal_layers);
  EXPECT_TRUE(plan.discardable_enhancement_layer);
}

TEST(StreamQualityTests, LowBudgetTextIntentFlagsPreferRoiAndLongTermReference) {
  auto plan = stream_quality::plan_low_bitrate_clarity({
    .width = 2560,
    .height = 1440,
    .fps = 60,
    .video_bitrate_kbps = 2500,
    .video_format = 1,
    .chroma_sampling_type = 1,
    .content_type = stream_quality::content_type_e::text,
  });

  EXPECT_NE(plan.intent_flags & stream_quality::clarity_intent_roi, 0U);
  EXPECT_NE(plan.intent_flags & stream_quality::clarity_intent_dirty_region, 0U);
  EXPECT_NE(plan.intent_flags & stream_quality::clarity_intent_long_term_reference, 0U);
  EXPECT_EQ(plan.intent_flags & stream_quality::clarity_intent_temporal_layers, 0U);
  EXPECT_EQ(plan.intent_flags & stream_quality::clarity_intent_discardable_enhancement, 0U);
}

TEST(StreamQualityTests, HighRefreshGameIntentFlagsPreferDirtyTemporalEnhancement) {
  auto plan = stream_quality::plan_low_bitrate_clarity({
    .width = 3024,
    .height = 1900,
    .fps = 120,
    .video_bitrate_kbps = 2340,
    .video_format = 1,
    .chroma_sampling_type = 0,
    .content_type = stream_quality::content_type_e::game,
  });

  EXPECT_NE(plan.intent_flags & stream_quality::clarity_intent_roi, 0U);
  EXPECT_NE(plan.intent_flags & stream_quality::clarity_intent_dirty_region, 0U);
  EXPECT_NE(plan.intent_flags & stream_quality::clarity_intent_temporal_layers, 0U);
  EXPECT_NE(plan.intent_flags & stream_quality::clarity_intent_discardable_enhancement, 0U);
  EXPECT_NE(plan.intent_flags & stream_quality::clarity_intent_intra_refresh, 0U);
}

TEST(StreamQualityTests, AllContentTypesCanGenerateInterestIntentUnderPressure) {
  for (auto content_type : {
         stream_quality::content_type_e::desktop,
         stream_quality::content_type_e::text,
         stream_quality::content_type_e::motion,
         stream_quality::content_type_e::game,
       }) {
    auto plan = stream_quality::plan_low_bitrate_clarity({
      .width = 3024,
      .height = 1900,
      .fps = 120,
      .video_bitrate_kbps = 2340,
      .video_format = 1,
      .chroma_sampling_type = 0,
      .content_type = content_type,
    });

    EXPECT_TRUE(plan.enabled);
    EXPECT_NE(plan.intent_flags & stream_quality::clarity_intent_dirty_region, 0U);
    EXPECT_NE(plan.intent_flags & stream_quality::clarity_intent_roi, 0U);
    EXPECT_NE(plan.intent_flags & stream_quality::clarity_intent_temporal_layers, 0U);
  }
}

TEST(StreamQualityTests, StaticKeepalivePreventsVrrFromStarvingTheClient) {
  EXPECT_EQ(stream_quality::static_frame_keepalive_fps(120, true, 0), 30);
  EXPECT_EQ(stream_quality::static_frame_keepalive_fps(96, true, 0), 24);
  EXPECT_EQ(stream_quality::static_frame_keepalive_fps(60, true, 0), 15);
  EXPECT_EQ(stream_quality::static_frame_keepalive_fps(30, true, 0), 8);
}

TEST(StreamQualityTests, StaticKeepaliveHonorsExplicitMinimumFpsTarget) {
  EXPECT_EQ(stream_quality::static_frame_keepalive_fps(120, true, 45), 45);
  EXPECT_EQ(stream_quality::static_frame_keepalive_fps(120, false, 45), 45);
  EXPECT_EQ(stream_quality::static_frame_keepalive_fps(30, true, 90), 30);
}
