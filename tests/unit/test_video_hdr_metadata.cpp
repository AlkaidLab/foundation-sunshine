/**
 * @file tests/unit/test_video_hdr_metadata.cpp
 * @brief Tests for HDR Vivid metadata generation and temporal filtering.
 */
#include <src/video_hdr_metadata.h>

#include <limits>

extern "C" {
#include <libavutil/hdr_dynamic_metadata.h>
#include <libavutil/rational.h>
}

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

  // config_t::videoFormat convention.
  constexpr int H264 = 0;
  constexpr int HEVC = 1;
  constexpr int AV1 = 2;

}  // namespace

TEST(HdrDynamicMetadata, RoutesFormatsByTransferFunction) {
  using video::colorspace_e;
  using video::hdr_metadata::formats_for;
  using video::sunshine_colorspace_t;

  // HDR10+ carries absolute luminance, so it only describes PQ. HDR Vivid covers
  // both PQ and HLG (T/UWA 005.1-2024 clause 7).
  const auto pq = formats_for(sunshine_colorspace_t { colorspace_e::bt2020, false, 10 }, HEVC);
  EXPECT_TRUE(pq.hdr10plus);
  EXPECT_TRUE(pq.vivid);

  const auto hlg = formats_for(sunshine_colorspace_t { colorspace_e::bt2020hlg, true, 10 }, HEVC);
  EXPECT_FALSE(hlg.hdr10plus);
  EXPECT_TRUE(hlg.vivid);

  const auto sdr = formats_for(sunshine_colorspace_t { colorspace_e::rec709, false, 8 }, HEVC);
  EXPECT_FALSE(sdr.hdr10plus);
  EXPECT_FALSE(sdr.vivid);
}

TEST(HdrDynamicMetadata, WithholdsVividFromCodecsWithoutACarriage) {
  using video::colorspace_e;
  using video::hdr_metadata::formats_for;
  using video::sunshine_colorspace_t;

  const sunshine_colorspace_t pq { colorspace_e::bt2020, false, 10 };
  const sunshine_colorspace_t hlg { colorspace_e::bt2020hlg, true, 10 };

  // The regression this guards: HDR Vivid must never reach an AV1 metadata OBU,
  // even though PQ content is eligible for it. HDR10+ has an AOMedia-defined AV1
  // carriage and stays.
  const auto av1_pq = formats_for(pq, AV1);
  EXPECT_TRUE(av1_pq.hdr10plus);
  EXPECT_FALSE(av1_pq.vivid);

  // HLG over AV1 therefore carries no dynamic metadata at all: HDR10+ is PQ-only
  // and HDR Vivid has no AV1 carriage.
  const auto av1_hlg = formats_for(hlg, AV1);
  EXPECT_FALSE(av1_hlg.hdr10plus);
  EXPECT_FALSE(av1_hlg.vivid);

  // H.264 is never used for HDR here, and claims no carriage rather than asserting
  // unverified AVC support.
  EXPECT_FALSE(formats_for(pq, H264).vivid);

  // HEVC is unchanged on every axis.
  EXPECT_TRUE(formats_for(pq, HEVC).vivid);
  EXPECT_TRUE(formats_for(hlg, HEVC).vivid);
}

TEST(HdrDynamicMetadata, SkipsVividPrerollWhenCodecCannotCarryIt) {
  using video::colorspace_e;
  using video::hdr_metadata::needs_vivid_startup_preroll;
  using video::sunshine_colorspace_t;

  const sunshine_colorspace_t hlg { colorspace_e::bt2020hlg, true, 10 };
  const sunshine_colorspace_t pq { colorspace_e::bt2020, false, 10 };

  // HLG over HEVC still waits for the startup guard, because Vivid really is sent
  // and a plain-HLG IDR followed by a mid-stream switch would be visible.
  EXPECT_TRUE(needs_vivid_startup_preroll(hlg, HEVC, true));

  // The regression this guards: AV1 has no Vivid carriage, so holding the first
  // frame for the guard's samples or its timeout would delay startup waiting on
  // metadata that is never emitted.
  EXPECT_FALSE(needs_vivid_startup_preroll(hlg, AV1, true));

  // PQ never prerolls: it has no HLG-to-Vivid startup transition.
  EXPECT_FALSE(needs_vivid_startup_preroll(pq, HEVC, true));

  // Without the analyzer there is nothing to stabilize, so no wait either.
  EXPECT_FALSE(needs_vivid_startup_preroll(hlg, HEVC, false));
}

TEST(HdrDynamicMetadata, SerializesAndRoundTripsHdr10PlusT35) {
  platf::hdr_frame_luminance_stats_t stats {};
  stats.percentile_95 = 500.0f;
  stats.avg_maxrgb = 100.0f;
  stats.valid = true;

  std::array<uint8_t, video::hdr_metadata::hdr10plus_t35_max_payload_size> payload {};
  const size_t payload_size = video::hdr_metadata::serialize_hdr10plus_t35(stats, 1000, payload);
  ASSERT_GT(payload_size, video::hdr_metadata::hdr10plus_t35_prefix_size);

  const std::vector<uint8_t> expected_prefix { 0xB5, 0x00, 0x3C, 0x00, 0x01, 0x04 };
  ASSERT_TRUE(std::equal(expected_prefix.begin(), expected_prefix.end(), payload.begin()));

  AVDynamicHDRPlus decoded {};
  ASSERT_GE(av_dynamic_hdr_plus_from_t35(
              &decoded, payload.data() + expected_prefix.size(),
              payload_size - expected_prefix.size()),
    0);
  EXPECT_EQ(decoded.application_version, video::hdr_metadata::hdr10plus_application_version);
  EXPECT_EQ(decoded.num_windows, 1);
  EXPECT_EQ(av_cmp_q(decoded.targeted_system_display_maximum_luminance, av_make_q(1000, 1)), 0);
  EXPECT_EQ(av_cmp_q(decoded.params[0].maxscl[0], av_make_q(50000, 100000)), 0);
  EXPECT_EQ(av_cmp_q(decoded.params[0].maxscl[1], av_make_q(50000, 100000)), 0);
  EXPECT_EQ(av_cmp_q(decoded.params[0].maxscl[2], av_make_q(50000, 100000)), 0);
  EXPECT_EQ(av_cmp_q(decoded.params[0].average_maxrgb, av_make_q(10000, 100000)), 0);
  EXPECT_EQ(decoded.params[0].tone_mapping_flag, 0);
  EXPECT_EQ(decoded.params[0].color_saturation_mapping_flag, 0);
}

TEST(HdrDynamicMetadata, RejectsInvalidHdr10PlusStatsAndOutputBuffers) {
  platf::hdr_frame_luminance_stats_t stats {};
  stats.percentile_95 = 400.0f;
  stats.avg_maxrgb = 80.0f;
  stats.valid = false;

  std::array<uint8_t, video::hdr_metadata::hdr10plus_t35_max_payload_size> payload {};
  EXPECT_EQ(video::hdr_metadata::serialize_hdr10plus_t35(stats, 1000, payload), 0U);

  stats.valid = true;
  std::array<uint8_t, 16> undersized_payload {};
  EXPECT_EQ(video::hdr_metadata::serialize_hdr10plus_t35(stats, 1000, undersized_payload), 0U);

  const auto rejected = [&](float percentile_95, float average_maxrgb) {
    stats.percentile_95 = percentile_95;
    stats.avg_maxrgb = average_maxrgb;
    return video::hdr_metadata::serialize_hdr10plus_t35(stats, 1000, payload) == 0;
  };
  EXPECT_TRUE(rejected(std::numeric_limits<float>::quiet_NaN(), 80.0f));
  EXPECT_TRUE(rejected(-1.0f, 80.0f));
  EXPECT_TRUE(rejected(400.0f, std::numeric_limits<float>::quiet_NaN()));
  EXPECT_TRUE(rejected(400.0f, -1.0f));
}

TEST(HdrDynamicMetadata, AppliesHdr10PlusTargetLuminanceFallbackAndClamp) {
  platf::hdr_frame_luminance_stats_t stats {};
  stats.percentile_95 = 400.0f;
  stats.avg_maxrgb = 80.0f;
  stats.valid = true;

  std::array<uint8_t, video::hdr_metadata::hdr10plus_t35_max_payload_size> payload {};
  size_t payload_size = video::hdr_metadata::serialize_hdr10plus_t35(stats, 0, payload);
  ASSERT_GT(payload_size, video::hdr_metadata::hdr10plus_t35_prefix_size);
  AVDynamicHDRPlus decoded {};
  ASSERT_GE(av_dynamic_hdr_plus_from_t35(
              &decoded,
              payload.data() + video::hdr_metadata::hdr10plus_t35_prefix_size,
              payload_size - video::hdr_metadata::hdr10plus_t35_prefix_size),
    0);
  EXPECT_EQ(av_cmp_q(decoded.targeted_system_display_maximum_luminance, av_make_q(1000, 1)), 0);

  payload_size = video::hdr_metadata::serialize_hdr10plus_t35(stats, 60000, payload);
  ASSERT_GT(payload_size, video::hdr_metadata::hdr10plus_t35_prefix_size);
  decoded = {};
  ASSERT_GE(av_dynamic_hdr_plus_from_t35(
              &decoded,
              payload.data() + video::hdr_metadata::hdr10plus_t35_prefix_size,
              payload_size - video::hdr_metadata::hdr10plus_t35_prefix_size),
    0);
  EXPECT_EQ(av_cmp_q(decoded.targeted_system_display_maximum_luminance, av_make_q(10000, 1)), 0);
}

TEST(HdrDynamicMetadata, SharesHdr10PlusNormalizationAcrossEncoderPaths) {
  const auto metadata = video::hdr_metadata::hdr10plus_from_luminance(500.0f, 100.0f, 1000);
  ASSERT_TRUE(metadata.valid);
  EXPECT_EQ(metadata.maxscl, 50000);
  EXPECT_EQ(metadata.average_maxrgb, 10000);
  EXPECT_EQ(metadata.targeted_system_display_maximum_luminance, 1000);

  const auto fallback = video::hdr_metadata::hdr10plus_from_luminance(400.0f, 80.0f, 0);
  ASSERT_TRUE(fallback.valid);
  EXPECT_EQ(fallback.targeted_system_display_maximum_luminance, 1000);
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

namespace {

  platf::hdr_frame_luminance_stats_t
  stable_hlg_stats(uint64_t sequence, float average = 120.0f, float maximum = 600.0f) {
    return {
      .min_maxrgb = 0.0f,
      .max_maxrgb = maximum,
      .avg_maxrgb = average,
      .percentile_10_pq = 0.20f,
      .percentile_90_pq = 0.70f,
      .percentile_95 = 500.0f,
      .percentile_99 = 580.0f,
      .analysis_max_nits = 1000.0f,
      .sample_sequence = sequence,
      .valid = true,
    };
  }

}  // namespace

TEST(HdrDynamicMetadata, VividStartupGuardRequiresThreeIndependentSamples) {
  video::hdr_metadata::vivid_startup_guard_t guard;

  const auto first = stable_hlg_stats(1);
  EXPECT_FALSE(guard.observe(first));
  EXPECT_EQ(guard.consecutive_samples(), 1U);

  // Reusing an analyzer result on intervening encoded frames must not satisfy
  // the startup guard.
  EXPECT_FALSE(guard.observe(first));
  EXPECT_EQ(guard.consecutive_samples(), 1U);

  EXPECT_FALSE(guard.observe(stable_hlg_stats(2, 125.0f, 620.0f)));
  EXPECT_EQ(guard.consecutive_samples(), 2U);

  // A non-adjacent replay is still the same GPU readback and must not count.
  EXPECT_FALSE(guard.observe(first));
  EXPECT_EQ(guard.consecutive_samples(), 2U);

  EXPECT_TRUE(guard.observe(stable_hlg_stats(3, 130.0f, 610.0f)));
}

TEST(HdrDynamicMetadata, VividStartupGuardRejectsInvalidAndTransitionSamples) {
  video::hdr_metadata::vivid_startup_guard_t guard;

  EXPECT_FALSE(guard.observe(stable_hlg_stats(1)));

  auto invalid = stable_hlg_stats(2);
  invalid.avg_maxrgb = std::numeric_limits<float>::quiet_NaN();
  EXPECT_FALSE(guard.observe(invalid));
  EXPECT_EQ(guard.consecutive_samples(), 0U);

  invalid = stable_hlg_stats(3);
  invalid.max_maxrgb = 1500.0f;
  EXPECT_FALSE(guard.observe(invalid));
  EXPECT_EQ(guard.consecutive_samples(), 0U);

  auto black = stable_hlg_stats(4, 0.0f, 0.0f);
  black.percentile_10_pq = 0.0f;
  black.percentile_90_pq = 0.0f;
  EXPECT_FALSE(guard.observe(black));
  EXPECT_EQ(guard.consecutive_samples(), 0U);

  EXPECT_FALSE(guard.observe(stable_hlg_stats(5)));
  // A large exposure transition restarts the consecutive run at the current sample.
  EXPECT_FALSE(guard.observe(stable_hlg_stats(6, 500.0f, 950.0f)));
  EXPECT_EQ(guard.consecutive_samples(), 1U);
  EXPECT_FALSE(guard.observe(stable_hlg_stats(7, 510.0f, 940.0f)));
  EXPECT_TRUE(guard.observe(stable_hlg_stats(8, 500.0f, 930.0f)));
}

// Regression guard for the representation of
// AVHDRVividColorToneMappingParams::targeted_system_display_maximum_luminance.
//
// FFmpeg parses that field as a 12-bit code with a fixed denominator of 4095
// (libavcodec/dynamic_hdr_vivid.c: `(AVRational){get_bits(gb, 12), maximum_luminance_den}`)
// and documents the value range as 0.0 to 1.0 inclusive. Writing raw nits with a
// denominator of 1 — as this code did before — yields values like 1000/1, far outside
// that range. Encode it as a PQ code value, consistently with the four maxrgb fields.
TEST(HdrDynamicMetadata, TargetDisplayLuminanceIsPqCodeNotNits) {
  for (const float nits : { 400.0f, 1000.0f, 4000.0f, 10000.0f }) {
    const auto code = video::hdr_metadata::pq_to_u12(video::hdr_metadata::nits_to_pq(nits));

    // Must land inside the 12-bit range the field is defined over.
    EXPECT_LE(code, 4095u) << "nits=" << nits;

    // A raw-nits encoding would exceed 4095 for every value above it, which is
    // precisely the bug this guards against.
    if (nits > 4095.0f) {
      EXPECT_LT(static_cast<float>(code), nits) << "nits=" << nits;
    }

    // pq_to_u12 truncates, so `code` is the floor of the exact PQ position. That
    // brackets the requested luminance: decoding `code` lands at or below it, and
    // decoding the next code lands above it. Only float round-trip error needs a
    // tolerance here — measured worst case is ~7.3e-07 in PQ, about 0.07 nits at the
    // top of the range — not a whole quantization step, which near 10000 nits is 23
    // nits wide and would let a genuinely wrong code pass.
    const float tolerance = std::max(nits * 1e-5f, 0.001f);
    const float decoded = video::hdr_metadata::pq_to_nits(
      static_cast<float>(code) / video::hdr_metadata::pq_u12_den);
    EXPECT_LE(decoded, nits + tolerance) << "nits=" << nits;

    if (code < video::hdr_metadata::pq_u12_den) {
      const float next = video::hdr_metadata::pq_to_nits(
        static_cast<float>(code + 1) / video::hdr_metadata::pq_u12_den);
      EXPECT_GT(next, nits - tolerance) << "nits=" << nits;
    }
    else {
      // Saturated at the top of the 12-bit range. 10000 nits is the PQ ceiling, so
      // there is no next code to bracket against and none should be read.
      EXPECT_NEAR(decoded, 10000.0f, tolerance) << "nits=" << nits;
    }
  }

  // PQ is monotonic, so ordering of target luminances must be preserved.
  EXPECT_LT(
    video::hdr_metadata::pq_to_u12(video::hdr_metadata::nits_to_pq(400.0f)),
    video::hdr_metadata::pq_to_u12(video::hdr_metadata::nits_to_pq(1000.0f)));
  EXPECT_EQ(
    video::hdr_metadata::pq_to_u12(video::hdr_metadata::nits_to_pq(10000.0f)), 4095u);
}

// target_display_pq_u12() is the single conversion used by both the frame-setup and
// per-frame metadata paths, so its <= 0 fallback is behavioral, not cosmetic: a display
// that reports no peak luminance must still yield the 1000-nit code both places agree on.
TEST(HdrDynamicMetadata, TargetDisplayHelperMatchesManualChainAndFallsBackTo1000Nits) {
  using video::hdr_metadata::nits_to_pq;
  using video::hdr_metadata::pq_to_u12;
  using video::hdr_metadata::target_display_pq_u12;

  // Matches the manual nits -> PQ -> 12-bit chain it replaces.
  for (const float nits : { 1.0f, 400.0f, 1000.0f, 4000.0f, 10000.0f }) {
    EXPECT_EQ(target_display_pq_u12(nits), pq_to_u12(nits_to_pq(nits))) << "nits=" << nits;
  }

  // Unreported / invalid peaks collapse to the documented 1000-nit default.
  const auto fallback = pq_to_u12(nits_to_pq(1000.0f));
  EXPECT_EQ(target_display_pq_u12(0.0f), fallback);
  EXPECT_EQ(target_display_pq_u12(-1.0f), fallback);
  EXPECT_EQ(target_display_pq_u12(-10000.0f), fallback);

  // The denominator the codes are paired with is what FFmpeg parses them against.
  EXPECT_EQ(video::hdr_metadata::pq_u12_den, 4095);
  EXPECT_LE(target_display_pq_u12(10000.0f), video::hdr_metadata::pq_u12_den);
}
