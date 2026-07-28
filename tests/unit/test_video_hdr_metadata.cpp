/**
 * @file tests/unit/test_video_hdr_metadata.cpp
 * @brief Tests for HDR Vivid metadata generation and temporal filtering.
 */
#include <src/video_hdr_metadata.h>

#include <limits>

#include "../tests_common.h"

namespace {

  uint32_t
  read_bits(const std::vector<uint8_t> &data, size_t &bit_offset, int bit_count) {
    uint32_t value = 0;
    for (int bit = 0; bit < bit_count; ++bit) {
      const size_t byte_index = bit_offset / 8;
      const int bit_index = 7 - static_cast<int>(bit_offset % 8);
      value = (value << 1) | ((data.at(byte_index) >> bit_index) & 1U);
      ++bit_offset;
    }
    return value;
  }

}  // namespace

TEST(HdrDynamicMetadata, RoutesFormatsByTransferFunction) {
  using video::colorspace_e;
  using video::hdr_metadata::formats_for;
  using video::sunshine_colorspace_t;

  const auto pq = formats_for(sunshine_colorspace_t { colorspace_e::bt2020, false, 10 });
  EXPECT_TRUE(pq.hdr10plus);
  EXPECT_TRUE(pq.vivid);

  const auto hlg = formats_for(sunshine_colorspace_t { colorspace_e::bt2020hlg, true, 10 });
  EXPECT_FALSE(hlg.hdr10plus);
  EXPECT_TRUE(hlg.vivid);

  const auto sdr = formats_for(sunshine_colorspace_t { colorspace_e::rec709, false, 8 });
  EXPECT_FALSE(sdr.hdr10plus);
  EXPECT_FALSE(sdr.vivid);
}

TEST(HdrDynamicMetadata, GeneratesVividFieldsInPqContentDomain) {
  platf::hdr_frame_luminance_stats_t stats;
  stats.min_maxrgb = 0.0f;
  stats.avg_maxrgb = 100.0f;
  stats.max_maxrgb = 1000.0f;
  stats.percentile_10_pq = 0.1f;
  stats.percentile_90_pq = 0.9f;
  stats.percentile_95 = 400.0f;  // Must not replace the actual maximum.
  stats.valid = true;

  const auto metadata = video::hdr_metadata::vivid_from_stats(stats);
  ASSERT_TRUE(metadata.valid);
  EXPECT_EQ(metadata.minimum_maxrgb_pq, 0);
  EXPECT_EQ(metadata.average_maxrgb_pq,
    video::hdr_metadata::pq_to_u12(video::hdr_metadata::nits_to_pq(100.0f)));
  EXPECT_EQ(metadata.variance_maxrgb_pq,
    video::hdr_metadata::pq_to_u12(stats.percentile_90_pq - stats.percentile_10_pq));
  EXPECT_EQ(metadata.maximum_maxrgb_pq,
    video::hdr_metadata::pq_to_u12(video::hdr_metadata::nits_to_pq(1000.0f)));
}

TEST(HdrDynamicMetadata, PqConversionMatchesSt2084ReferencePoints) {
  EXPECT_NEAR(video::hdr_metadata::nits_to_pq(100.0f), 0.5080784f, 0.000001f);
  EXPECT_NEAR(video::hdr_metadata::nits_to_pq(1000.0f), 0.7518271f, 0.000001f);
  EXPECT_NEAR(video::hdr_metadata::pq_to_nits(
                video::hdr_metadata::nits_to_pq(1000.0f)),
    1000.0f,
    0.1f);
  EXPECT_EQ(video::hdr_metadata::pq_to_u12(
              std::numeric_limits<float>::quiet_NaN()),
    0);
}

TEST(HdrDynamicMetadata, SerializesVividStatisticsModeSyntax) {
  video::hdr_metadata::vivid_metadata_t metadata;
  metadata.minimum_maxrgb_pq = 1;
  metadata.average_maxrgb_pq = 2;
  metadata.variance_maxrgb_pq = 3;
  metadata.maximum_maxrgb_pq = 4;
  metadata.valid = true;

  std::vector<uint8_t> payload;
  ASSERT_EQ(video::hdr_metadata::serialize_vivid_t35(metadata, payload), 13U);

  const std::vector<uint8_t> expected_header { 0x26, 0x00, 0x04, 0x00, 0x05, 0x01 };
  ASSERT_TRUE(std::equal(expected_header.begin(), expected_header.end(), payload.begin()));

  // GB/T 46269.1-2025 Table 11 derives num_windows=1; it is not a syntax element.
  size_t bit_offset = expected_header.size() * 8;
  EXPECT_EQ(read_bits(payload, bit_offset, 12), 1U);
  EXPECT_EQ(read_bits(payload, bit_offset, 12), 2U);
  EXPECT_EQ(read_bits(payload, bit_offset, 12), 3U);
  EXPECT_EQ(read_bits(payload, bit_offset, 12), 4U);
  EXPECT_EQ(read_bits(payload, bit_offset, 1), 0U);  // tone_mapping_enable_mode_flag
  EXPECT_EQ(read_bits(payload, bit_offset, 1), 0U);  // color_saturation_mapping_enable_flag

  while (bit_offset < payload.size() * 8) {
    EXPECT_EQ(read_bits(payload, bit_offset, 1), 0U);
  }
}

TEST(HdrDynamicMetadata, AppliesAnnexA9ThirtyTwoFrameMean) {
  video::hdr_metadata::vivid_temporal_filter_t filter;

  video::hdr_metadata::vivid_metadata_t dark;
  dark.minimum_maxrgb_pq = 100;
  dark.average_maxrgb_pq = 100;
  dark.variance_maxrgb_pq = 100;
  dark.maximum_maxrgb_pq = 100;
  dark.valid = true;

  for (int frame = 0; frame < 32; ++frame) {
    EXPECT_EQ(filter.update(dark).average_maxrgb_pq, 100);
  }

  auto bright = dark;
  bright.minimum_maxrgb_pq = 3300;
  bright.average_maxrgb_pq = 3300;
  bright.variance_maxrgb_pq = 3300;
  bright.maximum_maxrgb_pq = 3300;

  // One bright frame replaces one of 32 dark frames.
  EXPECT_EQ(filter.update(bright).average_maxrgb_pq, 200);

  video::hdr_metadata::vivid_metadata_t filtered;
  for (int frame = 1; frame < 32; ++frame) {
    filtered = filter.update(bright);
  }
  EXPECT_EQ(filtered.average_maxrgb_pq, 3300);

  filter.reset();
  EXPECT_EQ(filter.update(dark).average_maxrgb_pq, 100);
}
