#include "src/platform/windows/d3d12/d3d12_hdr_statistics.h"

#include <gtest/gtest.h>

namespace {
  using namespace platf::dxgi::d3d12;

  TEST(D3D12HdrStatistics, EmptyResultIsInvalid) {
    EXPECT_FALSE(summarize_hdr_result({}).valid);
  }

  TEST(D3D12HdrStatistics, MatchesD3D11NearestRankSemantics) {
    hdr_final_result_t result;
    result.min_maxrgb = 12.0f;
    result.max_maxrgb = 1000.0f;
    result.sum_maxrgb = 2500.0f;
    result.pixel_count = 10;
    for (std::size_t index = 0; index < 10; ++index) {
      result.histogram[index] = 1;
    }

    const auto summary = summarize_hdr_result(result);
    ASSERT_TRUE(summary.valid);
    EXPECT_FLOAT_EQ(summary.min_maxrgb, 12.0f);
    EXPECT_FLOAT_EQ(summary.max_maxrgb, 1000.0f);
    EXPECT_FLOAT_EQ(summary.avg_maxrgb, 250.0f);
    EXPECT_FLOAT_EQ(summary.percentile_10_pq, 0.5f / 256.0f);
    EXPECT_FLOAT_EQ(summary.percentile_90_pq, 8.5f / 256.0f);
    EXPECT_FLOAT_EQ(summary.percentile_95_pq, 9.5f / 256.0f);
    EXPECT_FLOAT_EQ(summary.percentile_99_pq, 9.5f / 256.0f);
  }

  TEST(D3D12HdrStatistics, UsesWideHistogramAccumulator) {
    hdr_final_result_t result;
    result.pixel_count = 0xFFFFFFFFu;
    result.histogram[0] = 0xF0000000u;
    result.histogram[255] = 0x0FFFFFFFu;
    EXPECT_TRUE(summarize_hdr_result(result).valid);
  }
}  // namespace
