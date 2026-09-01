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

  // H.264 loses HDR Vivid like AV1 does. HDR10+ is not codec-gated, so its verdict
  // is unchanged there, matching the behaviour before this change — and nothing is
  // emitted either way, because the encoder paths only inject for HEVC and AV1.
  const auto h264_pq = formats_for(pq, H264);
  EXPECT_TRUE(h264_pq.hdr10plus);
  EXPECT_FALSE(h264_pq.vivid);

  // HEVC is unchanged on every axis.
  EXPECT_TRUE(formats_for(pq, HEVC).vivid);
  EXPECT_TRUE(formats_for(hlg, HEVC).vivid);
}

TEST(HdrDynamicMetadata, ReportsWhenNoFormatCanBeCarried) {
  using video::colorspace_e;
  using video::hdr_metadata::formats_for;
  using video::sunshine_colorspace_t;

  const sunshine_colorspace_t pq { colorspace_e::bt2020, false, 10 };
  const sunshine_colorspace_t hlg { colorspace_e::bt2020hlg, true, 10 };
  const sunshine_colorspace_t sdr { colorspace_e::rec709, false, 8 };

  // What the capture path gates per-frame luminance analysis on. HLG over AV1 is
  // HDR, so a colorspace check alone would keep analyzing for metadata that can
  // never be written.
  EXPECT_FALSE(formats_for(hlg, AV1).any());
  EXPECT_FALSE(formats_for(sdr, HEVC).any());
  EXPECT_FALSE(formats_for(sdr, AV1).any());

  EXPECT_TRUE(formats_for(pq, AV1).any());
  EXPECT_TRUE(formats_for(pq, HEVC).any());
  EXPECT_TRUE(formats_for(hlg, HEVC).any());
}

TEST(HdrDynamicMetadata, IntersectsStreamFormatsWithEncoderCapability) {
  using video::colorspace_e;
  using video::hdr_metadata::formats_for;
  using video::hdr_metadata::formats_t;
  using video::sunshine_colorspace_t;

  const sunshine_colorspace_t pq { colorspace_e::bt2020, false, 10 };
  const sunshine_colorspace_t hlg { colorspace_e::bt2020hlg, true, 10 };

  // What each encode device can write, independent of the stream. FFmpeg has no
  // encoder-side serializer for AV_FRAME_DATA_DYNAMIC_HDR_VIVID, so anything driven
  // through avcodec attaches that side data and drops it. The native NVENC and AMF
  // paths hand-write both T.35 payloads.
  constexpr formats_t avcodec { .hdr10plus = true, .vivid = false };
  constexpr formats_t native { .hdr10plus = true, .vivid = true };

  // HLG leaves avcodec nothing even on HEVC: HDR10+ cannot describe HLG, and the
  // one format that could is never written. Analysis there is pure overhead.
  EXPECT_FALSE(formats_for(hlg, HEVC).intersect(avcodec).any());

  // The same stream keeps HDR Vivid on the two native paths.
  EXPECT_TRUE(formats_for(hlg, HEVC).intersect(native).vivid);
  EXPECT_TRUE(formats_for(hlg, HEVC).intersect(native).any());

  // PQ still reaches avcodec as HDR10+, so its analysis is not gated off.
  EXPECT_TRUE(formats_for(pq, HEVC).intersect(avcodec).hdr10plus);
  EXPECT_FALSE(formats_for(pq, HEVC).intersect(avcodec).vivid);
  EXPECT_TRUE(formats_for(pq, AV1).intersect(avcodec).any());

  // Neither gate can add a format the other refused.
  EXPECT_FALSE(formats_for(hlg, AV1).intersect(native).any());
  EXPECT_FALSE(formats_for(pq, HEVC).intersect(formats_t {}).any());
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
  stats.percentile_99 = 500.0f;
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
  // 500 nits and 100 nits against the ST 2084 10000-nit reference peak. The
  // targeted display above is reported separately and must not scale these.
  EXPECT_EQ(av_cmp_q(decoded.params[0].maxscl[0], av_make_q(5000, 100000)), 0);
  EXPECT_EQ(av_cmp_q(decoded.params[0].maxscl[1], av_make_q(5000, 100000)), 0);
  EXPECT_EQ(av_cmp_q(decoded.params[0].maxscl[2], av_make_q(5000, 100000)), 0);
  EXPECT_EQ(av_cmp_q(decoded.params[0].average_maxrgb, av_make_q(1000, 100000)), 0);
  EXPECT_EQ(decoded.params[0].tone_mapping_flag, 0);
  EXPECT_EQ(decoded.params[0].color_saturation_mapping_flag, 0);
}

TEST(HdrDynamicMetadata, CarriesTheNineHdr10PlusPercentiles) {
  platf::hdr_frame_luminance_stats_t stats {};
  stats.percentile_99 = 1000.0f;  // The analyzer fills this from the CDF's top slot.
  stats.avg_maxrgb = 300.0f;
  const float nits[] = { 10, 50, 100, 200, 300, 500, 800, 900, 1000 };
  std::copy(std::begin(nits), std::end(nits), std::begin(stats.distribution_maxrgb));
  stats.valid = true;

  std::array<uint8_t, video::hdr_metadata::hdr10plus_t35_max_payload_size> payload {};
  const size_t payload_size = video::hdr_metadata::serialize_hdr10plus_t35(stats, 1000, payload);
  ASSERT_GT(payload_size, video::hdr_metadata::hdr10plus_t35_prefix_size);

  AVDynamicHDRPlus decoded {};
  ASSERT_GE(av_dynamic_hdr_plus_from_t35(
              &decoded,
              payload.data() + video::hdr_metadata::hdr10plus_t35_prefix_size,
              payload_size - video::hdr_metadata::hdr10plus_t35_prefix_size),
    0);

  // A zero count parses back cleanly but is not what shipping HDR10+ sends, which is
  // how the empty distribution survived the round-trip check before.
  ASSERT_EQ(decoded.params[0].num_distribution_maxrgb_percentiles,
    video::hdr_metadata::hdr10plus_percentages.size());
  for (size_t i = 0; i < video::hdr_metadata::hdr10plus_percentages.size(); ++i) {
    EXPECT_EQ(decoded.params[0].distribution_maxrgb[i].percentage,
      video::hdr_metadata::hdr10plus_percentages[i]);
  }
  // 10 nits and 1000 nits against the 10000-nit reference peak, in units of 1/100000.
  EXPECT_EQ(av_cmp_q(decoded.params[0].distribution_maxrgb[0].percentile,
              av_make_q(100, 100000)),
    0);
  EXPECT_EQ(av_cmp_q(decoded.params[0].distribution_maxrgb[8].percentile,
              av_make_q(10000, 100000)),
    0);

  // ST 2094-40 8.5.4: the 5% and 10% slots are reserved in application_version 1
  // and carry fixed values rather than analyzer percentiles.
  EXPECT_EQ(av_cmp_q(decoded.params[0].distribution_maxrgb[1].percentile,
              av_make_q(0, 100000)),
    0);
  EXPECT_EQ(av_cmp_q(decoded.params[0].distribution_maxrgb[2].percentile,
              av_make_q(255, 100000)),
    0);

  // The CDF must not decrease across the slots that are actually part of it.
  const size_t cdf_indices[] = { 0, 3, 4, 5, 6, 7, 8 };
  for (size_t i = 1; i < std::size(cdf_indices); ++i) {
    EXPECT_LE(av_cmp_q(decoded.params[0].distribution_maxrgb[cdf_indices[i - 1]].percentile,
                decoded.params[0].distribution_maxrgb[cdf_indices[i]].percentile),
      0);
  }
}

TEST(HdrDynamicMetadata, NormalizesHdr10PlusAgainstTheReferencePeakNotTheDisplay) {
  // The regression: normalizing content statistics against the target display
  // scaled every value by 10000/peak, so a client reading maxSCL back as
  // 10000 x the rational saw a picture up to ten times brighter than it was.
  // Only targeted_system_display_maximum_luminance may vary with the display.
  const auto at = [](uint16_t display_nits) {
    return video::hdr_metadata::hdr10plus_from_luminance(500.0f, 100.0f, display_nits);
  };

  for (const uint16_t display_nits : { uint16_t { 400 }, uint16_t { 1000 }, uint16_t { 4000 } }) {
    const auto metadata = at(display_nits);
    ASSERT_TRUE(metadata.valid);
    EXPECT_EQ(metadata.maxscl, 5000) << "display peak " << display_nits;
    EXPECT_EQ(metadata.average_maxrgb, 1000) << "display peak " << display_nits;
    EXPECT_EQ(metadata.targeted_system_display_maximum_luminance, display_nits);
  }

  // Content brighter than the display is content, not a clipped signal: it must
  // survive normalization instead of saturating at the display peak.
  const auto bright = video::hdr_metadata::hdr10plus_from_luminance(4000.0f, 100.0f, 400);
  ASSERT_TRUE(bright.valid);
  EXPECT_EQ(bright.maxscl, 40000);
}

TEST(HdrDynamicMetadata, SeparatesClientTargetPeakFromMasteringPeak) {
  EXPECT_EQ(video::hdr_metadata::resolve_target_display_luminance(true, 400.0f, 1000), 400);
  EXPECT_EQ(video::hdr_metadata::resolve_target_display_luminance(false, 400.0f, 1000), 1000);
  EXPECT_EQ(video::hdr_metadata::resolve_target_display_luminance(true, 20000.0f, 1000), 10000);
  EXPECT_EQ(video::hdr_metadata::resolve_target_display_luminance(true, 0.0f, 1000), 1000);
  EXPECT_EQ(video::hdr_metadata::resolve_target_display_luminance(false, 0.0f, 0), 1000);
}

TEST(HdrDynamicMetadata, ReservesTheHdr10PlusV1AndV2DistributionSlots) {
  // libplacebo reads a nonzero slot 1 as ST 2094-50 V1, the 99.99% scene
  // luminance, and prefers the CIE-Y values derived from it over maxSCL. Leaking
  // the 5th percentile there reports the frame's darkest region as its peak.
  const float nits[] = { 10, 50, 100, 200, 300, 500, 800, 900, 1000 };
  const auto metadata =
    video::hdr_metadata::hdr10plus_from_luminance(900.0f, 300.0f, 1000, nits);
  ASSERT_TRUE(metadata.valid);

  EXPECT_EQ(metadata.distribution_maxrgb[video::hdr_metadata::hdr10plus_reserved_v1_index],
    video::hdr_metadata::hdr10plus_reserved_v1);
  EXPECT_EQ(metadata.distribution_maxrgb[video::hdr_metadata::hdr10plus_reserved_v2_index],
    video::hdr_metadata::hdr10plus_reserved_v2);

  // Every other slot still carries the analyzer value.
  EXPECT_EQ(metadata.distribution_maxrgb[0], 100);
  EXPECT_EQ(metadata.distribution_maxrgb[3], 2000);
  EXPECT_EQ(metadata.distribution_maxrgb[8], 10000);
}

TEST(HdrDynamicMetadata, ZeroesTheWholeDistributionOnOneBadEntry) {
  using namespace video::hdr_metadata;

  // One unusable entry discards the whole CDF rather than shipping a partially
  // filled one. maxSCL and average_maxrgb survive, and V1 = 0 is what tells a
  // consumer to use them, so the frame is still worth sending.
  const auto check = [](const hdr10plus_frame_metadata_t &metadata) {
    EXPECT_TRUE(metadata.valid);
    EXPECT_GT(metadata.maxscl, 0);
    EXPECT_GT(metadata.average_maxrgb, 0);

    for (size_t i = 0; i < metadata.distribution_maxrgb.size(); ++i) {
      if (i == hdr10plus_reserved_v1_index || i == hdr10plus_reserved_v2_index) {
        continue;
      }
      EXPECT_EQ(metadata.distribution_maxrgb[i], 0) << "CDF slot " << i;
    }

    // 8.5.4 fixes these two in application_version 1 whether or not the analysis
    // was usable, so the rejection path must not zero V2 either.
    EXPECT_EQ(metadata.distribution_maxrgb[hdr10plus_reserved_v1_index], hdr10plus_reserved_v1);
    EXPECT_EQ(metadata.distribution_maxrgb[hdr10plus_reserved_v2_index], hdr10plus_reserved_v2);
  };

  float bad[] = { 10, 50, 100, std::numeric_limits<float>::quiet_NaN(), 300, 500, 800, 900, 1000 };
  check(hdr10plus_from_luminance(900.0f, 300.0f, 1000, bad));

  float negative[] = { 10, 50, 100, 200, -1.0f, 500, 800, 900, 1000 };
  check(hdr10plus_from_luminance(900.0f, 300.0f, 1000, negative));

  // A bad entry in a reserved slot is still a bad analysis: the CDF goes away
  // even though that slot's value would have been overwritten anyway.
  float bad_reserved[] = { 10, -1.0f, 100, 200, 300, 500, 800, 900, 1000 };
  check(hdr10plus_from_luminance(900.0f, 300.0f, 1000, bad_reserved));

  // Same story with no distribution at all. The serializer emits all nine
  // percentiles unconditionally, so the reserved slots have to be right even when
  // the caller never offered a CDF.
  check(hdr10plus_from_luminance(900.0f, 300.0f, 1000));
}

TEST(HdrDynamicMetadata, RejectsInvalidHdr10PlusStatsAndOutputBuffers) {
  platf::hdr_frame_luminance_stats_t stats {};
  stats.percentile_99 = 400.0f;
  stats.avg_maxrgb = 80.0f;
  stats.valid = false;

  std::array<uint8_t, video::hdr_metadata::hdr10plus_t35_max_payload_size> payload {};
  EXPECT_EQ(video::hdr_metadata::serialize_hdr10plus_t35(stats, 1000, payload), 0U);

  stats.valid = true;
  std::array<uint8_t, 16> undersized_payload {};
  EXPECT_EQ(video::hdr_metadata::serialize_hdr10plus_t35(stats, 1000, undersized_payload), 0U);

  const auto rejected = [&](float peak_maxrgb, float average_maxrgb) {
    stats.percentile_99 = peak_maxrgb;
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
  stats.percentile_99 = 400.0f;
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
  EXPECT_EQ(metadata.maxscl, 5000);
  EXPECT_EQ(metadata.average_maxrgb, 1000);
  EXPECT_EQ(metadata.targeted_system_display_maximum_luminance, 1000);

  const auto fallback = video::hdr_metadata::hdr10plus_from_luminance(400.0f, 80.0f, 0);
  ASSERT_TRUE(fallback.valid);
  EXPECT_EQ(fallback.targeted_system_display_maximum_luminance, 1000);
}

TEST(HdrDynamicMetadata, SmoothsHdr10PlusLuminanceAcrossFrames) {
  // The regression: the native NVENC path serialized raw analyzer output, so its
  // HDR10+ stepped at every GPU readback while the Vivid metadata built from the
  // same stats was already averaged by vivid_temporal_filter_t.
  const auto flat = [](float nits) {
    platf::hdr_frame_luminance_stats_t stats {};
    stats.min_maxrgb = nits;
    stats.max_maxrgb = nits;
    stats.avg_maxrgb = nits;
    stats.percentile_99 = nits;
    for (auto &entry : stats.distribution_maxrgb) {
      entry = nits;
    }
    stats.valid = true;
    return stats;
  };

  video::hdr_metadata::hdr_luminance_ema_t ema;
  EXPECT_FALSE(ema.initialized);

  // First sample snaps: there is nothing to blend against yet.
  ema.update(flat(100.0f));
  EXPECT_TRUE(ema.initialized);
  EXPECT_FLOAT_EQ(ema.percentile_99, 100.0f);

  // A 2x change stays under SCENE_CUT_THRESHOLD, so it blends at ALPHA.
  ema.update(flat(200.0f));
  EXPECT_FLOAT_EQ(ema.percentile_99, 115.0f);
  EXPECT_FLOAT_EQ(ema.avg_maxrgb, 115.0f);
  EXPECT_FLOAT_EQ(ema.distribution_maxrgb[0], 115.0f);

  // A scene cut past the threshold snaps instead of dragging the old scene along.
  ema.update(flat(2000.0f));
  EXPECT_FLOAT_EQ(ema.percentile_99, 2000.0f);

  ema.reset();
  EXPECT_FALSE(ema.initialized);
  EXPECT_FLOAT_EQ(ema.percentile_99, 0.0f);
}

TEST(HdrDynamicMetadata, DetectsDistributionSceneChangesOnlyOnNewSamples) {
  const auto scene = [](uint64_t sequence, float mean_pq, float distribution_nits) {
    platf::hdr_frame_luminance_stats_t stats {};
    stats.sample_sequence = sequence;
    stats.avg_maxrgb_pq = mean_pq;
    stats.percentile_10_pq = 0.10f;
    stats.percentile_90_pq = 0.60f;
    stats.max_maxrgb = 1000.0f;
    std::fill(std::begin(stats.distribution_maxrgb), std::end(stats.distribution_maxrgb), distribution_nits);
    stats.valid = true;
    return stats;
  };

  video::hdr_metadata::scene_change_detector_t detector;
  EXPECT_TRUE(detector.observe(scene(1, 0.30f, 100.0f)));

  // Four encoded frames may reuse one GPU readback. It must remain one event.
  EXPECT_FALSE(detector.observe(scene(1, 0.30f, 100.0f)));

  EXPECT_FALSE(detector.observe(scene(2, 0.32f, 110.0f)));

  // The exact peak is unchanged, but the luminance distribution belongs to a
  // different scene. Peak-ratio detection alone misses this transition.
  EXPECT_TRUE(detector.observe(scene(3, 0.33f, 1000.0f)));
}

TEST(HdrDynamicMetadata, InvalidPqSamplesDoNotPoisonTemporalState) {
  const auto scene = [](uint64_t sequence, float mean_pq, float nits) {
    platf::hdr_frame_luminance_stats_t stats {};
    stats.sample_sequence = sequence;
    stats.min_maxrgb = nits;
    stats.max_maxrgb = nits;
    stats.avg_maxrgb = nits;
    stats.avg_maxrgb_pq = mean_pq;
    stats.percentile_10_pq = mean_pq;
    stats.percentile_90_pq = mean_pq;
    stats.percentile_99 = nits;
    std::fill(std::begin(stats.distribution_maxrgb), std::end(stats.distribution_maxrgb), nits);
    stats.valid = true;
    return stats;
  };

  video::hdr_metadata::dynamic_metadata_temporal_state_t temporal;
  ASSERT_TRUE(temporal.update(scene(1, 0.20f, 10.0f)).vivid.valid);

  auto invalid = scene(2, 0.45f, 15.0f);
  invalid.percentile_10_pq = std::numeric_limits<float>::quiet_NaN();
  const auto rejected = temporal.update(invalid);
  EXPECT_FALSE(rejected.hdr10plus_stats.valid);
  EXPECT_FALSE(rejected.vivid.valid);

  // The rejected sample must neither become the scene baseline nor enter either
  // temporal filter. This cut therefore snaps both formats to the new scene.
  const auto cut = temporal.update(scene(3, 0.70f, 20.0f));
  EXPECT_FLOAT_EQ(cut.hdr10plus_stats.percentile_99, 20.0f);
  EXPECT_EQ(cut.vivid.average_maxrgb_pq, video::hdr_metadata::pq_to_u12(0.70f));
}

TEST(HdrDynamicMetadata, SceneChangeResetsTheVividWindow) {
  const auto scene = [](uint64_t sequence, float mean_pq, float nits) {
    platf::hdr_frame_luminance_stats_t stats {};
    stats.sample_sequence = sequence;
    stats.min_maxrgb = nits;
    stats.max_maxrgb = nits;
    stats.avg_maxrgb = nits;
    stats.avg_maxrgb_pq = mean_pq;
    stats.percentile_10_pq = mean_pq;
    stats.percentile_90_pq = mean_pq;
    stats.percentile_99 = nits;
    std::fill(std::begin(stats.distribution_maxrgb), std::end(stats.distribution_maxrgb), nits);
    stats.valid = true;
    return stats;
  };

  video::hdr_metadata::dynamic_metadata_builder_t builder;
  builder.configure({ .hdr10plus = false, .vivid = true });
  for (int frame = 0; frame < 32; ++frame) {
    ASSERT_FALSE(builder.build(scene(1, 0.20f, 10.0f), 1000).vivid.empty());
  }

  const auto payload = builder.build(scene(2, 0.70f, 1000.0f), 1000).vivid;
  ASSERT_FALSE(payload.empty());
  const std::vector<uint8_t> bytes(payload.begin(), payload.end());
  size_t bit_offset = 6 * 8 + 12;  // T.35 prefix + minimum_maxrgb
  EXPECT_EQ(read_bits(bytes, bit_offset, 12), video::hdr_metadata::pq_to_u12(0.70f));
}

TEST(HdrDynamicMetadata, SmoothedStatsSubstituteOnlyTheFilteredFields) {
  platf::hdr_frame_luminance_stats_t raw {};
  raw.avg_maxrgb = 100.0f;
  raw.max_maxrgb = 100.0f;
  raw.percentile_99 = 100.0f;
  raw.avg_maxrgb_pq = 0.35f;
  raw.percentile_10_pq = 0.25f;
  raw.percentile_90_pq = 0.75f;
  raw.analysis_max_nits = 10000.0f;
  raw.sample_sequence = 42;
  raw.valid = true;

  video::hdr_metadata::hdr_luminance_ema_t ema;

  // Before the first sample there is nothing to substitute, so raw passes through.
  const auto passthrough = ema.smoothed(raw);
  EXPECT_FLOAT_EQ(passthrough.percentile_99, 100.0f);

  ema.update(raw);
  auto next = raw;
  next.avg_maxrgb = 200.0f;
  next.max_maxrgb = 200.0f;
  next.percentile_99 = 200.0f;
  ema.update(next);

  const auto smoothed = ema.smoothed(next);
  EXPECT_FLOAT_EQ(smoothed.percentile_99, 115.0f);
  EXPECT_FLOAT_EQ(smoothed.avg_maxrgb, 115.0f);

  // Fields this filter does not own must survive untouched: the PQ-domain
  // statistics belong to HDR Vivid, which does its own averaging.
  EXPECT_FLOAT_EQ(smoothed.avg_maxrgb_pq, 0.35f);
  EXPECT_FLOAT_EQ(smoothed.percentile_10_pq, 0.25f);
  EXPECT_FLOAT_EQ(smoothed.percentile_90_pq, 0.75f);
  EXPECT_FLOAT_EQ(smoothed.analysis_max_nits, 10000.0f);
  EXPECT_EQ(smoothed.sample_sequence, 42U);
  EXPECT_TRUE(smoothed.valid);
}

TEST(HdrDynamicMetadata, GeneratesVividFieldsInPqContentDomain) {
  platf::hdr_frame_luminance_stats_t stats;
  stats.min_maxrgb = 0.0f;
  stats.avg_maxrgb = 100.0f;
  stats.max_maxrgb = 1000.0f;
  stats.avg_maxrgb_pq = 0.4f;
  stats.percentile_10_pq = 0.1f;
  stats.percentile_90_pq = 0.9f;
  stats.percentile_99 = 400.0f;  // Must not replace the actual maximum.
  stats.valid = true;

  const auto metadata = video::hdr_metadata::vivid_from_stats(stats);
  ASSERT_TRUE(metadata.valid);
  EXPECT_EQ(metadata.minimum_maxrgb_pq, 0);
  EXPECT_EQ(metadata.average_maxrgb_pq, video::hdr_metadata::pq_to_u12(0.4f));
  EXPECT_EQ(metadata.variance_maxrgb_pq,
    video::hdr_metadata::pq_to_u12(stats.percentile_90_pq - stats.percentile_10_pq));
  EXPECT_EQ(metadata.maximum_maxrgb_pq,
    video::hdr_metadata::pq_to_u12(video::hdr_metadata::nits_to_pq(1000.0f)));
}

TEST(HdrDynamicMetadata, TakesTheVividAverageInThePqDomain) {
  // The regression: the average was PQ(mean(nits)) while the variance beside it was
  // a difference of PQ percentiles. PQ is concave, so Jensen's inequality makes
  // PQ(mean) >= mean(PQ), and on a dark frame with small highlights the gap is most
  // of the range — the display then anchors its curve far above the picture.
  //
  // 99% of the frame at 1 nit with 1% at 1000 nits: the PQ-domain mean is
  // 0.99 x PQ(1) + 0.01 x PQ(1000) ~ 0.157, while the linear mean is 10.99 nits,
  // whose PQ value is ~0.303.
  const float pq_domain_mean =
    0.99f * video::hdr_metadata::nits_to_pq(1.0f) +
    0.01f * video::hdr_metadata::nits_to_pq(1000.0f);

  platf::hdr_frame_luminance_stats_t stats {};
  stats.min_maxrgb = 1.0f;
  stats.max_maxrgb = 1000.0f;
  stats.avg_maxrgb = 10.99f;  // The linear-light mean HDR10+ reports.
  stats.avg_maxrgb_pq = pq_domain_mean;
  stats.percentile_10_pq = 0.15f;
  stats.percentile_90_pq = 0.16f;
  stats.valid = true;

  const auto metadata = video::hdr_metadata::vivid_from_stats(stats);
  ASSERT_TRUE(metadata.valid);
  EXPECT_EQ(metadata.average_maxrgb_pq, video::hdr_metadata::pq_to_u12(pq_domain_mean));
  EXPECT_LT(metadata.average_maxrgb_pq,
    video::hdr_metadata::pq_to_u12(video::hdr_metadata::nits_to_pq(stats.avg_maxrgb)));
  // Not a rounding difference: the old mapping reported roughly twice the code value.
  EXPECT_GT(video::hdr_metadata::pq_to_u12(
              video::hdr_metadata::nits_to_pq(stats.avg_maxrgb)),
    metadata.average_maxrgb_pq * 3 / 2);

  // The min and max do commute with the transfer function, so they still come from
  // the nits fields.
  EXPECT_EQ(metadata.maximum_maxrgb_pq,
    video::hdr_metadata::pq_to_u12(video::hdr_metadata::nits_to_pq(1000.0f)));
}

TEST(HdrDynamicMetadata, WithholdsVividWithoutAPqDomainAverage) {
  // A stats struct that never had a PQ-domain mean filled in must not silently ship
  // a black average, and must not fall back to the linear mean this replaced.
  platf::hdr_frame_luminance_stats_t stats {};
  stats.min_maxrgb = 1.0f;
  stats.max_maxrgb = 1000.0f;
  stats.avg_maxrgb = 100.0f;
  stats.percentile_10_pq = 0.2f;
  stats.percentile_90_pq = 0.7f;
  stats.valid = true;

  EXPECT_FALSE(video::hdr_metadata::vivid_from_stats(stats).valid);

  stats.avg_maxrgb_pq = 0.5f / 256.0f;
  EXPECT_TRUE(video::hdr_metadata::vivid_from_stats(stats).valid);

  for (const float bad : { std::numeric_limits<float>::quiet_NaN(), -0.1f, 1.5f }) {
    stats.avg_maxrgb_pq = bad;
    EXPECT_FALSE(video::hdr_metadata::vivid_from_stats(stats).valid);
  }

  // A genuinely black frame is not a missing statistic: both means read zero, they
  // agree, and the metadata says the picture is black.
  platf::hdr_frame_luminance_stats_t black {};
  black.valid = true;
  const auto black_metadata = video::hdr_metadata::vivid_from_stats(black);
  EXPECT_TRUE(black_metadata.valid);
  EXPECT_EQ(black_metadata.average_maxrgb_pq, 0);
}

TEST(HdrDynamicMetadata, KeepsMaxSclAtOrAboveEveryCarriedPercentile) {
  using namespace video::hdr_metadata;

  // The regression: maxSCL was the 95th percentile while the message also carried a
  // 99th percentile above it, so a consumer reading maxSCL as the scene peak — as
  // libplacebo does — clipped highlights the same message described.
  const float nits[] = { 10, 50, 100, 200, 300, 500, 800, 900, 1000 };

  // The analyzer's own pairing: the peak is the CDF's top slot.
  const auto matched = hdr10plus_from_luminance(1000.0f, 300.0f, 1000, nits);
  ASSERT_TRUE(matched.valid);
  EXPECT_EQ(matched.maxscl, matched.distribution_maxrgb[8]);

  // And structurally, whatever a caller reports as the peak.
  const auto understated = hdr10plus_from_luminance(500.0f, 300.0f, 1000, nits);
  ASSERT_TRUE(understated.valid);
  for (size_t i = 0; i < understated.distribution_maxrgb.size(); ++i) {
    if (i == hdr10plus_reserved_v1_index || i == hdr10plus_reserved_v2_index) {
      continue;
    }
    EXPECT_LE(understated.distribution_maxrgb[i], understated.maxscl) << "CDF slot " << i;
  }

  // The fixed 8.5.4 sentinels are not measurements and must not raise it: an almost
  // black frame keeps its own peak rather than V2's 0.00255.
  const float dark[] = { 0, 0, 0, 0, 0, 0, 0, 0, 1 };
  const auto nearly_black = hdr10plus_from_luminance(1.0f, 0.5f, 1000, dark);
  ASSERT_TRUE(nearly_black.valid);
  EXPECT_EQ(nearly_black.maxscl, 10);
  EXPECT_EQ(nearly_black.distribution_maxrgb[hdr10plus_reserved_v2_index], hdr10plus_reserved_v2);
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
      .avg_maxrgb_pq = 0.45f,
      .percentile_10_pq = 0.20f,
      .percentile_90_pq = 0.70f,
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

  // Vivid's average comes out of the PQ-domain statistic, so a readback that never
  // filled it is not a sample the stream can start on.
  invalid = stable_hlg_stats(4);
  invalid.avg_maxrgb_pq = 0.0f;
  EXPECT_FALSE(guard.observe(invalid));
  EXPECT_EQ(guard.consecutive_samples(), 0U);

  auto black = stable_hlg_stats(5, 0.0f, 0.0f);
  black.percentile_10_pq = 0.0f;
  black.percentile_90_pq = 0.0f;
  EXPECT_FALSE(guard.observe(black));
  EXPECT_EQ(guard.consecutive_samples(), 0U);

  // Only the PQ-domain average moves: the linear mean and the peak are unchanged, so
  // this is exactly the sample the other stability checks cannot catch.
  auto pq_jump = stable_hlg_stats(6);
  pq_jump.avg_maxrgb_pq = 0.80f;
  EXPECT_FALSE(guard.observe(stable_hlg_stats(7)));
  EXPECT_EQ(guard.consecutive_samples(), 1U);
  EXPECT_FALSE(guard.observe(pq_jump));
  EXPECT_EQ(guard.consecutive_samples(), 1U);

  EXPECT_FALSE(guard.observe(stable_hlg_stats(8)));
  // A large exposure transition restarts the consecutive run at the current sample.
  EXPECT_FALSE(guard.observe(stable_hlg_stats(9, 500.0f, 950.0f)));
  EXPECT_EQ(guard.consecutive_samples(), 1U);
  EXPECT_FALSE(guard.observe(stable_hlg_stats(10, 510.0f, 940.0f)));
  EXPECT_TRUE(guard.observe(stable_hlg_stats(11, 500.0f, 930.0f)));
}

namespace {

  using gate_t = video::hdr_metadata::vivid_startup_gate_t;

  constexpr video::sunshine_colorspace_t HLG { video::colorspace_e::bt2020hlg, false, 10 };
  constexpr video::sunshine_colorspace_t PQ { video::colorspace_e::bt2020, false, 10 };

}  // namespace

TEST(HdrDynamicMetadata, VividStartupGateEmitsImmediatelyForPq) {
  // PQ carries HDR10+ independently of the HLG/Vivid startup policy, so an
  // unavailable analyzer must never route it through plain-HLG fallback.
  gate_t gate { PQ, HEVC, true };
  EXPECT_FALSE(gate.prerolling());

  const auto now = std::chrono::steady_clock::now();
  const auto first = gate.observe(stable_hlg_stats(1), now);
  EXPECT_EQ(first.decision, gate_t::decision_e::emit);
  EXPECT_EQ(first.transition, gate_t::transition_e::none);

  gate_t without_analysis { PQ, HEVC, false };
  const auto still_emitted = without_analysis.observe({}, now + gate_t::PREROLL_TIMEOUT * 2);
  EXPECT_EQ(still_emitted.decision, gate_t::decision_e::emit);
  EXPECT_EQ(still_emitted.transition, gate_t::transition_e::none);
}

TEST(HdrDynamicMetadata, VividStartupGateHoldsHlgUntilTheGuardConverges) {
  gate_t gate { HLG, HEVC, true };
  EXPECT_TRUE(gate.prerolling());

  const auto start = std::chrono::steady_clock::now();
  for (uint64_t sequence = 1; sequence <= 2; ++sequence) {
    const auto held = gate.observe(stable_hlg_stats(sequence), start);
    EXPECT_EQ(held.decision, gate_t::decision_e::hold) << "sequence=" << sequence;
    EXPECT_EQ(held.transition, gate_t::transition_e::none) << "sequence=" << sequence;
  }

  // The third independent sample flips the gate and asks the caller for an IDR.
  const auto ready = gate.observe(stable_hlg_stats(3), start);
  EXPECT_EQ(ready.decision, gate_t::decision_e::emit);
  EXPECT_EQ(ready.transition, gate_t::transition_e::ready);
  EXPECT_EQ(gate.consecutive_samples(), video::hdr_metadata::vivid_startup_guard_t::REQUIRED_SAMPLES);
  EXPECT_FALSE(gate.prerolling());

  // Once open the gate stays open, and reports the transition only once. The
  // timeout no longer applies.
  const auto later = gate.observe(stable_hlg_stats(4), start + gate_t::PREROLL_TIMEOUT * 4);
  EXPECT_EQ(later.decision, gate_t::decision_e::emit);
  EXPECT_EQ(later.transition, gate_t::transition_e::none);
}

TEST(HdrDynamicMetadata, VividStartupGateAllowsAnalyzerColdStart) {
  gate_t gate { HLG, HEVC, true };

  const auto start = std::chrono::steady_clock::now();
  auto invalid = stable_hlg_stats(1);
  invalid.valid = false;

  // A slow first analyzer readback must not force this session into plain-HLG
  // fallback before the GPU has had a chance to produce valid samples.
  EXPECT_EQ(gate.observe(invalid, start).decision, gate_t::decision_e::hold);
  EXPECT_EQ(gate.observe(invalid, start + std::chrono::seconds { 1 }).decision,
    gate_t::decision_e::hold);

  EXPECT_EQ(gate.observe(stable_hlg_stats(2), start + std::chrono::seconds { 1 }).decision,
    gate_t::decision_e::hold);
  EXPECT_EQ(gate.observe(stable_hlg_stats(3), start + std::chrono::seconds { 1 }).decision,
    gate_t::decision_e::hold);
  const auto ready = gate.observe(stable_hlg_stats(4), start + std::chrono::seconds { 1 });
  EXPECT_EQ(ready.decision, gate_t::decision_e::emit);
  EXPECT_EQ(ready.transition, gate_t::transition_e::ready);
}

TEST(HdrDynamicMetadata, VividStartupGateFallsBackAndRecoversAfterTheTimeout) {
  gate_t gate { HLG, HEVC, true };

  const auto start = std::chrono::steady_clock::now();
  auto invalid = stable_hlg_stats(1);
  invalid.valid = false;
  EXPECT_EQ(gate.observe(invalid, start).decision, gate_t::decision_e::hold);
  // One millisecond short of the budget is still a hold.
  EXPECT_EQ(gate.observe(invalid, start + gate_t::PREROLL_TIMEOUT - std::chrono::milliseconds { 1 }).decision,
    gate_t::decision_e::hold);

  const auto expired = gate.observe(invalid, start + gate_t::PREROLL_TIMEOUT);
  EXPECT_EQ(expired.decision, gate_t::decision_e::plain_hlg);
  EXPECT_EQ(expired.transition, gate_t::transition_e::timed_out);

  // Replayed encoder frames must not fabricate analyzer progress during fallback.
  const auto first = stable_hlg_stats(2);
  EXPECT_EQ(gate.observe(first, start + gate_t::PREROLL_TIMEOUT * 2).decision,
    gate_t::decision_e::plain_hlg);
  EXPECT_EQ(gate.observe(first, start + gate_t::PREROLL_TIMEOUT * 3).decision,
    gate_t::decision_e::plain_hlg);
  EXPECT_EQ(gate.consecutive_samples(), 1U);

  EXPECT_EQ(gate.observe(stable_hlg_stats(3), start + gate_t::PREROLL_TIMEOUT * 4).decision,
    gate_t::decision_e::plain_hlg);
  const auto recovered = gate.observe(
    stable_hlg_stats(4), start + gate_t::PREROLL_TIMEOUT * 5);
  EXPECT_EQ(recovered.decision, gate_t::decision_e::emit);
  EXPECT_EQ(recovered.transition, gate_t::transition_e::recovered);

  // Recovery is a one-shot transition; the gate remains enabled afterward.
  const auto after = gate.observe(stable_hlg_stats(5), start + gate_t::PREROLL_TIMEOUT * 6);
  EXPECT_EQ(after.decision, gate_t::decision_e::emit);
  EXPECT_EQ(after.transition, gate_t::transition_e::none);
}

TEST(HdrDynamicMetadata, VividStartupGateKeepsSamplesAcrossFallback) {
  gate_t gate { HLG, HEVC, true };

  const auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(gate.observe(stable_hlg_stats(1), start).decision, gate_t::decision_e::hold);

  // The sample that arrives exactly at the deadline still counts before the gate
  // temporarily opens as plain HLG.
  const auto expired = gate.observe(stable_hlg_stats(2), start + gate_t::PREROLL_TIMEOUT);
  EXPECT_EQ(expired.decision, gate_t::decision_e::plain_hlg);
  EXPECT_EQ(expired.transition, gate_t::transition_e::timed_out);
  EXPECT_EQ(gate.consecutive_samples(), 2U);

  const auto recovered = gate.observe(
    stable_hlg_stats(3), start + gate_t::PREROLL_TIMEOUT + std::chrono::milliseconds { 1 });
  EXPECT_EQ(recovered.decision, gate_t::decision_e::emit);
  EXPECT_EQ(recovered.transition, gate_t::transition_e::recovered);
}

TEST(HdrDynamicMetadata, VividStartupGateDisablesHlgWithNothingToWaitFor) {
  const auto now = std::chrono::steady_clock::now();

  // No analyzer: HLG can never carry Vivid, so holding frames would only delay
  // the first frame by the full timeout.
  gate_t without_analysis { HLG, HEVC, false };
  EXPECT_FALSE(without_analysis.prerolling());
  EXPECT_EQ(without_analysis.observe(stable_hlg_stats(1), now).decision, gate_t::decision_e::disabled);

  // AV1 has no HDR Vivid carriage at all.
  gate_t av1 { HLG, AV1, true };
  EXPECT_FALSE(av1.prerolling());
  EXPECT_EQ(av1.observe(stable_hlg_stats(1), now).decision, gate_t::decision_e::disabled);
}

TEST(HdrDynamicMetadata, DynamicMetadataBuilderFollowsTheFormatGates) {
  const auto stats = stable_hlg_stats(1);

  // Unconfigured: nothing is emitted, even from valid stats.
  video::hdr_metadata::dynamic_metadata_builder_t builder;
  EXPECT_TRUE(builder.build(stats, 1000).empty());

  // PQ over HEVC carries both formats.
  builder.configure(video::hdr_metadata::formats_for(PQ, HEVC));
  const auto both = builder.build(stats, 1000);
  EXPECT_FALSE(both.hdr10plus.empty());
  EXPECT_FALSE(both.vivid.empty());
  // Both are registered T.35 payloads, distinguished by their country code.
  EXPECT_EQ(both.hdr10plus.front(), 0xB5);  // United States (SMPTE ST 2094-40)
  EXPECT_EQ(both.vivid.front(), 0x26);  // China (CUVA)

  // HLG carries Vivid only: HDR10+ describes absolute luminance.
  builder.configure(video::hdr_metadata::formats_for(HLG, HEVC));
  const auto vivid_only = builder.build(stats, 1000);
  EXPECT_TRUE(vivid_only.hdr10plus.empty());
  EXPECT_FALSE(vivid_only.vivid.empty());

  // AV1 has no Vivid carriage, so PQ over AV1 is HDR10+ only (see #930).
  builder.configure(video::hdr_metadata::formats_for(PQ, AV1));
  const auto hdr10plus_only = builder.build(stats, 1000);
  EXPECT_FALSE(hdr10plus_only.hdr10plus.empty());
  EXPECT_TRUE(hdr10plus_only.vivid.empty());

  // A display peak of zero is a real reading (DXGI reports MaxLuminance verbatim
  // and the VDD path clamps to a 0 floor), not a reason to stop emitting: HDR10+
  // falls back to a 1000-nit targeted system display and Vivid never reads it.
  builder.configure(video::hdr_metadata::formats_for(PQ, HEVC));
  const auto zero_peak = builder.build(stats, 0);
  EXPECT_FALSE(zero_peak.hdr10plus.empty());
  EXPECT_FALSE(zero_peak.vivid.empty());

  // Invalid analyzer output emits nothing rather than a frame of zeros.
  auto invalid = stats;
  invalid.valid = false;
  EXPECT_TRUE(builder.build(invalid, 1000).empty());
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
