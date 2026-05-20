/**
 * @file tests/unit/test_stream.cpp
 * @brief Test src/stream.*
 */

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "src/stream_quality_controller.h"

namespace stream {
  std::vector<uint8_t>
  concat_and_insert(uint64_t insert_size, uint64_t slice_size, const std::string_view &data1, const std::string_view &data2);

  struct runtime_profile_resolution_t {
    int width;
    int height;
  };

  runtime_profile_resolution_t
  runtime_profile_resolution_for_scale(int source_width, int source_height, int scale_percent);

  bool
  runtime_profile_resolution_reconfig_enabled();

  bool
  clipboard_transfer_length_valid(uint8_t item_type, std::uint32_t total_length);

  bool
  clipboard_transfer_chunk_next_length(std::uint32_t received_length,
                                       std::uint32_t total_length,
                                       std::uint32_t chunk_offset,
                                       std::uint16_t chunk_length,
                                       std::uint32_t &next_received_length);

  bool
  should_synthesize_stream_quality_recovery_feedback(int ml_feature_flags);

  bool
  should_apply_frame_fec_stream_quality_feedback(int ml_feature_flags);

  bool
  stream_quality_resync_guard_allows_safety_apply(const stream_quality::action_t &action,
                                           int last_applied_bitrate_kbps,
                                           int last_applied_fec_percentage);
}

#include "../tests_common.h"

TEST(ConcatAndInsertTests, ConcatNoInsertionTest) {
  char b1[] = { 'a', 'b' };
  char b2[] = { 'c', 'd', 'e' };
  auto res = stream::concat_and_insert(0, 2, std::string_view { b1, sizeof(b1) }, std::string_view { b2, sizeof(b2) });
  auto expected = std::vector<uint8_t> { 'a', 'b', 'c', 'd', 'e' };
  ASSERT_EQ(res, expected);
}

TEST(ConcatAndInsertTests, ConcatLargeStrideTest) {
  char b1[] = { 'a', 'b' };
  char b2[] = { 'c', 'd', 'e' };
  auto res = stream::concat_and_insert(1, sizeof(b1) + sizeof(b2) + 1, std::string_view { b1, sizeof(b1) }, std::string_view { b2, sizeof(b2) });
  auto expected = std::vector<uint8_t> { 0, 'a', 'b', 'c', 'd', 'e' };
  ASSERT_EQ(res, expected);
}

TEST(ConcatAndInsertTests, ConcatSmallStrideTest) {
  char b1[] = { 'a', 'b' };
  char b2[] = { 'c', 'd', 'e' };
  auto res = stream::concat_and_insert(1, 1, std::string_view { b1, sizeof(b1) }, std::string_view { b2, sizeof(b2) });
  auto expected = std::vector<uint8_t> { 0, 'a', 0, 'b', 0, 'c', 0, 'd', 0, 'e' };
  ASSERT_EQ(res, expected);
}

TEST(ClipboardTransferValidationTests, RejectsOversizedTextAndUnknownTypes) {
  constexpr std::uint32_t one_megabyte = 1024U * 1024U;

  EXPECT_TRUE(stream::clipboard_transfer_length_valid(LI_CLIPBOARD_ITEM_TYPE_NONE, 0));
  EXPECT_FALSE(stream::clipboard_transfer_length_valid(LI_CLIPBOARD_ITEM_TYPE_NONE, 1));
  EXPECT_TRUE(stream::clipboard_transfer_length_valid(LI_CLIPBOARD_ITEM_TYPE_IMAGE, LI_CLIPBOARD_MAX_IMAGE_SIZE));
  EXPECT_FALSE(stream::clipboard_transfer_length_valid(LI_CLIPBOARD_ITEM_TYPE_IMAGE, LI_CLIPBOARD_MAX_IMAGE_SIZE + 1));
  EXPECT_TRUE(stream::clipboard_transfer_length_valid(LI_CLIPBOARD_ITEM_TYPE_TEXT, one_megabyte));
  EXPECT_FALSE(stream::clipboard_transfer_length_valid(LI_CLIPBOARD_ITEM_TYPE_TEXT, one_megabyte + 1));
  EXPECT_FALSE(stream::clipboard_transfer_length_valid(0xFE, 16));
}

TEST(ClipboardTransferValidationTests, AcceptsSequentialChunksOnly) {
  std::uint32_t next_received_length = 0;

  EXPECT_TRUE(stream::clipboard_transfer_chunk_next_length(0, 10, 0, 4, next_received_length));
  EXPECT_EQ(next_received_length, 4U);
  EXPECT_TRUE(stream::clipboard_transfer_chunk_next_length(4, 10, 4, 6, next_received_length));
  EXPECT_EQ(next_received_length, 10U);
  EXPECT_TRUE(stream::clipboard_transfer_chunk_next_length(10, 10, 10, 0, next_received_length));
  EXPECT_EQ(next_received_length, 10U);

  EXPECT_FALSE(stream::clipboard_transfer_chunk_next_length(4, 10, 6, 2, next_received_length));
  EXPECT_FALSE(stream::clipboard_transfer_chunk_next_length(4, 10, 2, 2, next_received_length));
  EXPECT_FALSE(stream::clipboard_transfer_chunk_next_length(8, 10, 8, 3, next_received_length));
}

TEST(StreamQualityRecoveryFeedbackTests, DisablesSyntheticLossWhenNetworkFeedbackIsAvailable) {
  constexpr int networkFeedbackFeatureFlag = 0x20;

  EXPECT_FALSE(stream::should_synthesize_stream_quality_recovery_feedback(networkFeedbackFeatureFlag));
  EXPECT_TRUE(stream::should_synthesize_stream_quality_recovery_feedback(0));
}

TEST(StreamQualityRecoveryFeedbackTests, UsesNetworkFeedbackInsteadOfPerFrameFecWhenAvailable) {
  constexpr int networkFeedbackFeatureFlag = 0x20;

  EXPECT_FALSE(stream::should_apply_frame_fec_stream_quality_feedback(networkFeedbackFeatureFlag));
  EXPECT_TRUE(stream::should_apply_frame_fec_stream_quality_feedback(0));
}

TEST(StreamQualityRecoveryFeedbackTests, ResyncGuardAllowsCongestionAntiSpiralDownshift) {
  stream_quality::action_t action {};
  action.changed = true;
  action.state = stream_quality::state_e::crisis;
  action.reason = stream_quality::reason_e::random_loss;
  action.target_bitrate_kbps = 9000;
  action.fec_percentage = 20;
  action.congestion_anti_spiral = true;

  EXPECT_TRUE(stream::stream_quality_resync_guard_allows_safety_apply(action, 30000, 35));

  action.congestion_anti_spiral = false;
  EXPECT_FALSE(stream::stream_quality_resync_guard_allows_safety_apply(action, 30000, 35));

  action.congestion_anti_spiral = true;
  action.target_bitrate_kbps = 36000;
  action.fec_percentage = 40;
  EXPECT_FALSE(stream::stream_quality_resync_guard_allows_safety_apply(action, 30000, 35));
}

TEST(RuntimeProfileTierTests, ScalesBaseDimensionsToEvenTargets) {
  auto scaled = stream::runtime_profile_resolution_for_scale(3840, 2160, 75);
  EXPECT_EQ(scaled.width, 2880);
  EXPECT_EQ(scaled.height, 1620);

  auto full = stream::runtime_profile_resolution_for_scale(3840, 2160, 100);
  EXPECT_EQ(full.width, 3840);
  EXPECT_EQ(full.height, 2160);
}

TEST(RuntimeProfileTierTests, KeepsRuntimeEncoderResolutionReconfigurationDisabled) {
  EXPECT_FALSE(stream::runtime_profile_resolution_reconfig_enabled());
}
