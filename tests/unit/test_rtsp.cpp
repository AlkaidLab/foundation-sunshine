/**
 * @file tests/unit/test_rtsp.cpp
 * @brief Test RTSP stream setup helpers.
 */

#include <cstdint>

#include "../tests_common.h"

namespace rtsp_stream {
  std::uint64_t
  foundation_streaming_feature_flags2();

  int
  effective_stream_fec_percentage_for_client(int configured_fec_percentage, int ml_feature_flags);

  std::int64_t
  adjust_configured_video_bitrate_kbps(std::int64_t configured_bitrate_kbps,
                                       int fec_percentage,
                                       bool high_quality_audio,
                                       int audio_channels);
}  // namespace rtsp_stream

TEST(RtspFeatureFlags2Tests, AdvertisesOnlyWiredEnhancedRtpCapabilities) {
  constexpr std::uint64_t qosFeedbackV2 = 1ULL << 0;
  constexpr std::uint64_t inputPriorityV1 = 1ULL << 2;
  constexpr std::uint64_t cursorPlaneV1 = 1ULL << 3;
  constexpr std::uint64_t ft2QuicDatagramV1 = 1ULL << 7;

  auto flags = rtsp_stream::foundation_streaming_feature_flags2();

  EXPECT_NE(flags & qosFeedbackV2, 0U);
  EXPECT_NE(flags & inputPriorityV1, 0U);
  EXPECT_EQ(flags & cursorPlaneV1, 0U);
  EXPECT_EQ(flags & ft2QuicDatagramV1, 0U);
}

TEST(RtspBitrateAdjustmentTests, UsesLowStartupFecForFeedbackClients) {
  constexpr int networkFeedbackFeatureFlag = 0x20;

  auto effective_fec = rtsp_stream::effective_stream_fec_percentage_for_client(80, networkFeedbackFeatureFlag);
  EXPECT_LE(effective_fec, 12);

  auto adjusted_bitrate = rtsp_stream::adjust_configured_video_bitrate_kbps(
    10000,
    effective_fec,
    true,
    12);

  EXPECT_GE(adjusted_bitrate, 8500);
}

TEST(RtspBitrateAdjustmentTests, CapsConfiguredFecForLegacyClientsWithoutFeedback) {
  auto effective_fec = rtsp_stream::effective_stream_fec_percentage_for_client(80, 0);
  EXPECT_LE(effective_fec, 35);

  auto adjusted_bitrate = rtsp_stream::adjust_configured_video_bitrate_kbps(
    10000,
    effective_fec,
    true,
    12);

  EXPECT_GE(adjusted_bitrate, 5000);
}
