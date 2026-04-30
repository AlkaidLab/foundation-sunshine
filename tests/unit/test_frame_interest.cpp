/**
 * @file tests/unit/test_frame_interest.cpp
 * @brief Test frame interest map normalization and backend decisions.
 */

#include "src/frame_interest.h"
#include "src/stream_quality.h"

#include <gtest/gtest.h>

TEST(FrameInterestTests, ClampsAndMergesDirtyRectsInsideFrame) {
  frame_interest::map_t map;
  map.frame_width = 1920;
  map.frame_height = 1080;

  frame_interest::add_dirty_rect(map, { -20, 10, 120, 80 });
  frame_interest::add_dirty_rect(map, { 80, 20, 120, 90 });
  frame_interest::add_dirty_rect(map, { 4000, 4000, 20, 20 });
  frame_interest::finalize(map);

  ASSERT_EQ(map.dirty_rects.size(), 1U);
  EXPECT_EQ(map.dirty_rects[0].x, 0);
  EXPECT_EQ(map.dirty_rects[0].y, 10);
  EXPECT_EQ(map.dirty_rects[0].width, 200);
  EXPECT_EQ(map.dirty_rects[0].height, 100);
}

TEST(FrameInterestTests, KeepsMoveRectsAndCursorRoiBounded) {
  frame_interest::map_t map;
  map.frame_width = 1280;
  map.frame_height = 720;

  frame_interest::add_move_rect(map, { { 1200, 680, 200, 80 }, 900, 620 });
  frame_interest::add_cursor_roi(map, 12, 16, 64, -4);
  frame_interest::finalize(map);

  ASSERT_EQ(map.move_rects.size(), 1U);
  EXPECT_EQ(map.move_rects[0].dest.x, 1200);
  EXPECT_EQ(map.move_rects[0].dest.y, 680);
  EXPECT_EQ(map.move_rects[0].dest.width, 80);
  EXPECT_EQ(map.move_rects[0].dest.height, 40);

  ASSERT_EQ(map.roi_rects.size(), 1U);
  EXPECT_EQ(map.roi_rects[0].rect.x, 0);
  EXPECT_EQ(map.roi_rects[0].rect.y, 0);
  EXPECT_EQ(map.roi_rects[0].qp_delta, -4);
}

TEST(FrameInterestTests, BackendDecisionSeparatesAcceptedAndFallbackCapabilities) {
  frame_interest::map_t map;
  map.frame_width = 1920;
  map.frame_height = 1080;
  frame_interest::add_dirty_rect(map, { 100, 100, 300, 200 });
  frame_interest::add_cursor_roi(map, 960, 540, 96, -5);
  map.temporal_policy = frame_interest::temporal_policy_e::base_with_discardable_enhancement;
  frame_interest::finalize(map);

  frame_interest::backend_caps_t caps {
    .roi_qp_map = false,
    .dirty_rects = true,
    .move_rects = false,
    .temporal_layers = false,
    .long_term_reference = true,
    .intra_refresh = true,
    .adaptive_quantization = true,
  };

  auto decision = frame_interest::decide_backend(
    map,
    caps,
    stream_quality::clarity_intent_roi |
      stream_quality::clarity_intent_dirty_region |
      stream_quality::clarity_intent_temporal_layers);

  EXPECT_FALSE(decision.roi_accepted);
  EXPECT_TRUE(decision.roi_fallback);
  EXPECT_TRUE(decision.dirty_rects_accepted);
  EXPECT_FALSE(decision.temporal_layers_accepted);
  EXPECT_TRUE(decision.temporal_layers_fallback);
  EXPECT_TRUE(decision.uses_ltr_fallback);
  EXPECT_TRUE(decision.uses_intra_refresh_fallback);
  EXPECT_TRUE(decision.uses_aq_fallback);
}

TEST(FrameInterestTests, BackendDecisionAcceptsTemporalLayersWhenBackendSupportsSvc) {
  frame_interest::map_t map;
  map.frame_width = 3840;
  map.frame_height = 2160;
  map.temporal_policy = frame_interest::temporal_policy_e::base_with_discardable_enhancement;
  frame_interest::finalize(map);

  const auto decision = frame_interest::decide_backend(
    map,
    {
      .roi_qp_map = true,
      .dirty_rects = true,
      .move_rects = false,
      .temporal_layers = true,
      .long_term_reference = true,
      .intra_refresh = true,
      .adaptive_quantization = false,
    },
    stream_quality::clarity_intent_temporal_layers |
      stream_quality::clarity_intent_discardable_enhancement);

  EXPECT_TRUE(decision.temporal_layers_accepted);
  EXPECT_FALSE(decision.temporal_layers_fallback);
  EXPECT_FALSE(decision.uses_ltr_fallback);
  EXPECT_FALSE(decision.uses_intra_refresh_fallback);
  EXPECT_FALSE(decision.uses_aq_fallback);
}

TEST(FrameInterestTests, FullFrameDirtyRectIsClassifiedAsMotionInsteadOfDirtyRegionSaving) {
  frame_interest::map_t map;
  map.frame_width = 1920;
  map.frame_height = 1080;
  frame_interest::add_dirty_rect(map, { 0, 0, 1920, 1080 });
  frame_interest::finalize(map);

  EXPECT_TRUE(frame_interest::has_full_frame_dirty_region(map));

  const auto decision = frame_interest::decide_backend(
    map,
    {
      .roi_qp_map = true,
      .dirty_rects = true,
      .move_rects = true,
      .temporal_layers = false,
      .long_term_reference = true,
      .intra_refresh = true,
      .adaptive_quantization = true,
    },
    stream_quality::clarity_intent_dirty_region);

  EXPECT_FALSE(decision.dirty_rects_accepted);
  EXPECT_FALSE(decision.dirty_rects_fallback);
}

TEST(FrameInterestTests, FullFrameDirtyStillBuildsRoiQpDeltaMapWithoutDirtySavings) {
  frame_interest::map_t map;
  map.frame_width = 128;
  map.frame_height = 64;
  frame_interest::add_dirty_rect(map, { 0, 0, 128, 64 });
  frame_interest::add_roi_rect(map, { 32, 0, 64, 32 }, -6, 50);
  frame_interest::finalize(map);

  auto qp_map = frame_interest::build_qp_delta_map(
    map,
    32,
    stream_quality::clarity_intent_roi | stream_quality::clarity_intent_dirty_region);

  ASSERT_TRUE(qp_map.valid());
  ASSERT_EQ(qp_map.deltas.size(), 8U);
  EXPECT_EQ(qp_map.deltas[0], 0);
  EXPECT_EQ(qp_map.deltas[1], -6);
  EXPECT_EQ(qp_map.deltas[2], -6);
  EXPECT_EQ(qp_map.deltas[3], 0);
}

TEST(FrameInterestTests, QpDeltaMapPolicyHonorsExplicitEnableFlag) {
  auto policy = frame_interest::decide_qp_delta_map_policy(
    stream_quality::clarity_intent_roi | stream_quality::clarity_intent_dirty_region,
    false,
    true);

  EXPECT_FALSE(policy.enabled);
  EXPECT_FALSE(policy.disable_adaptive_quantization);
  EXPECT_TRUE(policy.fallback_to_adaptive_quantization);

  policy = frame_interest::decide_qp_delta_map_policy(
    stream_quality::clarity_intent_roi,
    true,
    true);

  EXPECT_TRUE(policy.enabled);
  EXPECT_TRUE(policy.disable_adaptive_quantization);
  EXPECT_FALSE(policy.fallback_to_adaptive_quantization);
}

TEST(FrameInterestTests, RuntimeDynamicInterestDoesNotArmQpMapBeforePressureAppears) {
  const auto flags = frame_interest::encoder_qp_delta_interest_flags(
    stream_quality::clarity_intent_long_term_reference,
    true);

  EXPECT_EQ(flags & stream_quality::clarity_intent_roi, 0U);
  EXPECT_EQ(flags & stream_quality::clarity_intent_dirty_region, 0U);
  EXPECT_NE(flags & stream_quality::clarity_intent_long_term_reference, 0U);

  auto policy = frame_interest::decide_qp_delta_map_policy(flags, true, true);
  EXPECT_FALSE(policy.enabled);
  EXPECT_FALSE(policy.disable_adaptive_quantization);
}

TEST(FrameInterestTests, BuildsBlockQpDeltaMapFromDirtyAndRoiRegions) {
  frame_interest::map_t map;
  map.frame_width = 128;
  map.frame_height = 64;
  frame_interest::add_dirty_rect(map, { 0, 0, 32, 32 });
  frame_interest::add_roi_rect(map, { 32, 0, 64, 32 }, -5, 50);
  frame_interest::finalize(map);

  auto qp_map = frame_interest::build_qp_delta_map(
    map,
    32,
    stream_quality::clarity_intent_roi | stream_quality::clarity_intent_dirty_region);

  ASSERT_TRUE(qp_map.valid());
  EXPECT_EQ(qp_map.blocks_wide, 4);
  EXPECT_EQ(qp_map.blocks_high, 2);
  ASSERT_EQ(qp_map.deltas.size(), 8U);
  EXPECT_EQ(qp_map.deltas[0], -1);
  EXPECT_EQ(qp_map.deltas[1], -5);
  EXPECT_EQ(qp_map.deltas[2], -5);
  EXPECT_EQ(qp_map.deltas[3], 0);
  EXPECT_EQ(qp_map.deltas[4], 0);
}

TEST(FrameInterestTests, NvencHevcQpDeltaMapUsesCtbGridForOddClientResolution) {
  frame_interest::map_t map;
  map.frame_width = 3024;
  map.frame_height = 1964;
  frame_interest::add_dirty_rect(map, { 0, 0, 1512, 982 });
  frame_interest::finalize(map);

  const auto block_size = frame_interest::nvenc_qp_delta_block_size_for_video_format(1);
  auto qp_map = frame_interest::build_qp_delta_map(
    map,
    block_size,
    stream_quality::clarity_intent_dirty_region);

  EXPECT_EQ(block_size, 32);
  EXPECT_EQ(qp_map.blocks_wide, 95);
  EXPECT_EQ(qp_map.blocks_high, 62);
  EXPECT_EQ(qp_map.deltas.size(), 5890U);
}
