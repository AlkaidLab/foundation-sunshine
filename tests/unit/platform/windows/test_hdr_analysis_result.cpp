/**
 * @file tests/unit/platform/windows/test_hdr_analysis_result.cpp
 * @brief Golden metadata checks for the shared D3D11/D3D12 result decoder.
 */
#ifdef _WIN32
  #include "src/platform/windows/hdr_analysis_result.h"
  #include "src/video_hdr_metadata.h"

  #include <gtest/gtest.h>
  #include <limits>

namespace {
  namespace analysis = platf::dxgi::hdr_analysis;

  analysis::result_t
  fixture() {
    analysis::result_t result;
    result.pixel_count = 100;
    result.min_maxrgb = 0;
    result.max_maxrgb = 10000;
    result.sum_maxrgb = 10000;
    result.sum_maxrgb_pq = 80;
    result.histogram[0] = 1;
    result.histogram[64] = 9;
    result.histogram[128] = 80;
    result.histogram[255] = 10;
    return result;
  }

  TEST(HdrAnalysisResult, EmptyReadbackDoesNotPublishMetadata) {
    const auto stats = analysis::decode_result({}, 10000, 7);
    EXPECT_FALSE(stats.valid);
    EXPECT_FALSE(stats.near_black_stats_valid);
    EXPECT_EQ(stats.sample_sequence, 0);
  }

  TEST(HdrAnalysisResult, PreservesPqMeanDistributionAndNearBlackCoverage) {
    const auto stats = analysis::decode_result(fixture(), 4000, 7);
    ASSERT_TRUE(stats.valid);
    EXPECT_EQ(stats.sample_sequence, 7);
    EXPECT_FLOAT_EQ(stats.analysis_max_nits, 4000);
    EXPECT_FLOAT_EQ(stats.min_maxrgb, 0);
    EXPECT_FLOAT_EQ(stats.max_maxrgb, 10000);
    EXPECT_FLOAT_EQ(stats.avg_maxrgb, 100);
    EXPECT_FLOAT_EQ(stats.avg_maxrgb_pq, 0.8f);
    EXPECT_TRUE(stats.near_black_stats_valid);
    EXPECT_FLOAT_EQ(stats.near_black_fraction, 0.01f);
    EXPECT_FLOAT_EQ(stats.percentile_1_pq, 0.5f / 256);
    EXPECT_FLOAT_EQ(stats.percentile_10_pq, 64.5f / 256);
    EXPECT_FLOAT_EQ(stats.percentile_90_pq, 128.5f / 256);
    constexpr int expected_bins[] { 0, 64, 64, 128, 128, 128, 128, 255, 255 };
    for (std::size_t i = 0; i < std::size(expected_bins); ++i) {
      EXPECT_FLOAT_EQ(stats.distribution_maxrgb[i],
        video::hdr_metadata::pq_to_nits((expected_bins[i] + 0.5f) / 256));
    }
    EXPECT_FLOAT_EQ(stats.percentile_99, stats.distribution_maxrgb[8]);
  }

  TEST(HdrAnalysisResult, InvalidPqMeanDoesNotDiscardHdr10PlusStatistics) {
    auto raw = fixture();
    raw.sum_maxrgb_pq = std::numeric_limits<float>::quiet_NaN();
    const auto stats = analysis::decode_result(raw, 4000, 8);
    EXPECT_TRUE(stats.valid);
    EXPECT_FLOAT_EQ(stats.avg_maxrgb_pq, 0);
    EXPECT_FLOAT_EQ(stats.avg_maxrgb, 100);
    EXPECT_GT(stats.percentile_99, 0);
    EXPECT_EQ(stats.sample_sequence, 8);
  }

  TEST(HdrAnalysisResult, ClampsOnlySmallPqRoundingOvershoot) {
    auto raw = fixture();
    raw.sum_maxrgb_pq = 100.05f;
    EXPECT_FLOAT_EQ(analysis::decode_result(raw, 4000, 9).avg_maxrgb_pq, 1);
    raw.sum_maxrgb_pq = 110;
    EXPECT_FLOAT_EQ(analysis::decode_result(raw, 4000, 10).avg_maxrgb_pq, 0);
  }
}  // namespace
#endif
