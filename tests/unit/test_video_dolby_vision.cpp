/**
 * @file tests/unit/test_video_dolby_vision.cpp
 * @brief Tests for the Dolby Vision Profile 8.1 RPU writer.
 *
 * The parser below is written independently from the writer — a separate bit
 * reader following the dovi_tool syntax — so a round trip proves the emitted
 * layout rather than merely the writer's own assumptions. The CRC check value
 * ("123456789" -> 0x0376E6E7) pins the test-local CRC-32/MPEG-2 to the
 * standard, which then cross-checks the writer's trailer.
 */
#include <src/video_dolby_vision.h>

#include "../tests_common.h"

#include <limits>
#include <vector>

namespace {

  using video::dolby_vision::clamp_level1;
  using video::dolby_vision::frame_metadata_from_stats;
  using video::dolby_vision::frame_metadata_t;
  using video::dolby_vision::rpu_generator_t;
  using video::dolby_vision::session_config_t;
  using video::dolby_vision::staged_rpu_queue_t;

  using bytes_t = std::vector<uint8_t>;

  /// CRC-32/MPEG-2, written independently of the implementation.
  uint32_t
  test_crc32_mpeg2(const uint8_t *data, size_t size) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < size; ++i) {
      crc ^= data[i] << 24;
      for (int bit = 0; bit < 8; ++bit) {
        const uint32_t msb = crc & 0x80000000;
        crc <<= 1;
        if (msb) crc ^= 0x04C11DB7;
      }
    }
    return crc;
  }

  class bit_reader_t {
  public:
    explicit bit_reader_t(const bytes_t &data):
        data_(data) {
    }

    uint32_t
    bits(int count) {
      uint32_t value = 0;
      for (int i = 0; i < count; ++i) {
        value = (value << 1) | read_bit();
      }
      return value;
    }

    uint64_t
    ue() {
      int zeros = 0;
      while (read_bit() == 0) {
        EXPECT_LT(zeros, 64);
        ++zeros;
      }
      uint64_t value = 1;
      for (int i = 0; i < zeros; ++i) {
        value = (value << 1) | read_bit();
      }
      return value - 1;
    }

    int64_t
    se() {
      const uint64_t mapped = ue();
      return (mapped & 1) ? static_cast<int64_t>((mapped + 1) / 2) : -static_cast<int64_t>(mapped / 2);
    }

    /// Consume the zero bits up to the next byte boundary, asserting they are zero.
    void
    skip_align_zeros() {
      while (position_ % 8 != 0) {
        EXPECT_EQ(read_bit(), 0u);
      }
    }

    /// Consume a fixed block-relative padding: the ext_dm_alignment_zero_bit
    /// after L1/L5 is the block's byte size minus its required bits (4 bits
    /// for both here), NOT a byte alignment of the stream.
    void
    skip_zero_bits(int count) {
      for (int i = 0; i < count; ++i) {
        EXPECT_EQ(read_bit(), 0u);
      }
    }

    size_t
    bit_position() const {
      return position_;
    }

    bool
    eof() const {
      return position_ == data_.size() * 8;
    }

  private:
    uint32_t
    read_bit() {
      EXPECT_LT(position_, data_.size() * 8);
      const uint8_t byte = data_[position_ / 8];
      const uint32_t bit = (byte >> (7 - position_ % 8)) & 1;
      ++position_;
      return bit;
    }

    const bytes_t &data_;
    size_t position_ = 0;
  };

  struct parsed_rpu_t {
    uint8_t profile = 0;
    uint8_t level = 0;
    uint64_t scene_refresh = 0;
    uint16_t source_min_pq = 0;
    uint16_t source_max_pq = 0;
    uint16_t l1_min = 0;
    uint16_t l1_max = 0;
    uint16_t l1_avg = 0;
    uint16_t l5[4] {};
    uint16_t l6[4] {};
    uint32_t crc = 0;
    size_t crc_byte_offset = 0;
  };

  /// Strip emulation prevention the way a decoder does.
  bytes_t
  unescape(std::span<const uint8_t> payload) {
    bytes_t out;
    int zero_run = 0;
    for (const uint8_t byte : payload) {
      if (zero_run == 2 && byte == 0x03) {
        zero_run = 0;
        continue;
      }
      out.push_back(byte);
      zero_run = (byte == 0x00) ? zero_run + 1 : 0;
    }
    return out;
  }

  /// Parse a generated NAL (0x7C 0x01 + escaped RPU) field by field.
  parsed_rpu_t
  parse_nal(std::span<const uint8_t> nal) {
    parsed_rpu_t parsed;
    EXPECT_GE(nal.size(), 3u);
    EXPECT_EQ(nal[0], 0x7C);  // nal_unit_type 62, layer 0
    EXPECT_EQ(nal[1], 0x01);  // temporal id +1 == 1

    const bytes_t rpu = unescape(nal.subspan(2));
    bit_reader_t reader { rpu };

    EXPECT_EQ(reader.bits(8), 0x19u);  // RPU prefix byte
    EXPECT_EQ(reader.bits(6), 2u);     // rpu_type
    EXPECT_EQ(reader.bits(11), 18u);   // rpu_format
    parsed.profile = reader.bits(4);
    parsed.level = reader.bits(4);
    EXPECT_EQ(reader.bits(1), 1u);  // vdr_seq_info_present_flag
    EXPECT_EQ(reader.bits(1), 0u);  // chroma_resampling_explicit_filter_flag
    EXPECT_EQ(reader.bits(2), 0u);  // coefficient_data_type
    EXPECT_EQ(reader.ue(), 23u);    // coefficient_log2_denom
    EXPECT_EQ(reader.bits(2), 1u);  // vdr_rpu_normalized_idc
    EXPECT_EQ(reader.bits(1), 0u);  // bl_video_full_range_flag
    EXPECT_EQ(reader.ue(), 2u);     // bl_bit_depth_minus8
    EXPECT_EQ(reader.ue(), 2u);     // el_bit_depth_minus8
    EXPECT_EQ(reader.ue(), 4u);     // vdr_bit_depth_minus8
    EXPECT_EQ(reader.bits(1), 0u);  // spatial_resampling_filter_flag
    EXPECT_EQ(reader.bits(3), 0u);  // reserved_zero_3bits
    EXPECT_EQ(reader.bits(1), 0u);  // el_spatial_resampling_filter_flag
    EXPECT_EQ(reader.bits(1), 1u);  // disable_residual_flag: no EL, profile 8
    EXPECT_EQ(reader.bits(1), 1u);  // vdr_dm_metadata_present_flag: RPU present
    EXPECT_EQ(reader.bits(1), 0u);  // use_prev_vdr_rpu_flag

    EXPECT_EQ(reader.ue(), 0u);  // vdr_rpu_id
    EXPECT_EQ(reader.ue(), 0u);  // mapping_color_space
    EXPECT_EQ(reader.ue(), 0u);  // mapping_chroma_format_idc
    for (int component = 0; component < 3; ++component) {
      EXPECT_EQ(reader.ue(), 0u);       // num_pivots_minus2
      EXPECT_EQ(reader.bits(10), 0u);   // pivot[0]
      EXPECT_EQ(reader.bits(10), 1023u);  // pivot[1]
    }
    EXPECT_EQ(reader.ue(), 0u);  // num_x_partitions_minus1
    EXPECT_EQ(reader.ue(), 0u);  // num_y_partitions_minus1
    for (int component = 0; component < 3; ++component) {
      EXPECT_EQ(reader.ue(), 0u);        // mapping_idc: polynomial
      EXPECT_EQ(reader.ue(), 0u);        // poly_order_minus1
      EXPECT_EQ(reader.bits(1), 0u);     // linear_interp_flag
      EXPECT_EQ(reader.se(), 0);         // poly_coef_int[0]
      EXPECT_EQ(reader.bits(23), 0u);    // poly_coef[0]
      EXPECT_EQ(reader.se(), 1);         // poly_coef_int[1]: identity curve
      EXPECT_EQ(reader.bits(23), 0u);    // poly_coef[1]
    }

    EXPECT_EQ(reader.ue(), 0u);  // affected_dm_metadata_id
    EXPECT_EQ(reader.ue(), 0u);  // current_dm_metadata_id
    parsed.scene_refresh = reader.ue();
    static const int16_t ycc_to_rgb_coef[9] = { 9574, 0, 13802, 9574, -1540, -5348, 9574, 17610, 0 };
    for (const int16_t coef : ycc_to_rgb_coef) {
      EXPECT_EQ(static_cast<int16_t>(reader.bits(16)), coef);
    }
    EXPECT_EQ(reader.bits(32), 16777216u);
    EXPECT_EQ(reader.bits(32), 134217728u);
    EXPECT_EQ(reader.bits(32), 134217728u);
    static const int16_t rgb_to_lms_coef[9] = { 7222, 8771, 390, 2654, 12430, 1300, 0, 422, 15962 };
    for (const int16_t coef : rgb_to_lms_coef) {
      EXPECT_EQ(static_cast<int16_t>(reader.bits(16)), coef);
    }
    EXPECT_EQ(reader.bits(16), 65535u);  // signal_eotf: ST 2084
    EXPECT_EQ(reader.bits(16), 0u);
    EXPECT_EQ(reader.bits(16), 0u);
    EXPECT_EQ(reader.bits(32), 0u);
    EXPECT_EQ(reader.bits(5), 12u);  // signal_bit_depth
    EXPECT_EQ(reader.bits(2), 0u);   // signal_color_space
    EXPECT_EQ(reader.bits(2), 0u);   // signal_chroma_format
    EXPECT_EQ(reader.bits(2), 1u);   // signal_full_range_flag
    parsed.source_min_pq = reader.bits(12);
    parsed.source_max_pq = reader.bits(12);
    EXPECT_EQ(reader.bits(10), 42u);  // source_diagonal

    EXPECT_EQ(reader.ue(), 3u);  // num_ext_blocks: L1, L5, L6
    reader.skip_align_zeros();

    EXPECT_EQ(reader.ue(), 5u);  // L1 ext_block_length
    EXPECT_EQ(reader.bits(8), 1u);
    parsed.l1_min = reader.bits(12);
    parsed.l1_max = reader.bits(12);
    parsed.l1_avg = reader.bits(12);
    reader.skip_zero_bits(4);  // L1: 5 bytes - 36 required bits

    EXPECT_EQ(reader.ue(), 7u);  // L5 ext_block_length
    EXPECT_EQ(reader.bits(8), 5u);
    for (auto &offset : parsed.l5) {
      offset = reader.bits(13);
    }
    reader.skip_zero_bits(4);  // L5: 7 bytes - 52 required bits

    EXPECT_EQ(reader.ue(), 8u);  // L6 ext_block_length
    EXPECT_EQ(reader.bits(8), 6u);
    for (auto &value : parsed.l6) {
      value = reader.bits(16);
    }
    reader.skip_align_zeros();

    parsed.crc_byte_offset = reader.bit_position() / 8;
    parsed.crc = reader.bits(32);
    EXPECT_EQ(reader.bits(8), 0x80u);  // rpu trailing byte
    EXPECT_TRUE(reader.eof());

    // The trailer must be the CRC-32/MPEG-2 of everything after 0x19.
    EXPECT_EQ(parsed.crc, test_crc32_mpeg2(rpu.data() + 1, parsed.crc_byte_offset - 1));
    return parsed;
  }

  frame_metadata_t
  typical_metadata() {
    return { .min_pq = 9, .max_pq = 3200, .avg_pq = 1500, .scene_refresh = false };
  }

}  // namespace

TEST(DolbyVisionRpu, Crc32Mpeg2CheckValue) {
  const char *check = "123456789";
  EXPECT_EQ(test_crc32_mpeg2(reinterpret_cast<const uint8_t *>(check), 9), 0x0376E6E7u);
}

TEST(DolbyVisionRpu, ClampsLevel1LikeDoviTool) {
  const auto floor_clamped = clamp_level1(0, 0, 0);
  EXPECT_EQ(floor_clamped.min_pq, 0);
  EXPECT_EQ(floor_clamped.max_pq, video::dolby_vision::l1_max_pq_min);
  EXPECT_EQ(floor_clamped.avg_pq, video::dolby_vision::l1_avg_pq_min);

  const auto ceiling_clamped = clamp_level1(4095, 4095, 4095);
  EXPECT_EQ(ceiling_clamped.min_pq, video::dolby_vision::l1_min_pq_max);
  EXPECT_EQ(ceiling_clamped.max_pq, video::dolby_vision::l1_max_pq_max);
  EXPECT_EQ(ceiling_clamped.avg_pq, video::dolby_vision::l1_max_pq_max - 1);

  // avg stays within [819, max-1] but is otherwise untouched.
  const auto middle = clamp_level1(999, 100, 999);
  EXPECT_EQ(middle.min_pq, 12);
  EXPECT_EQ(middle.max_pq, 2081);
  EXPECT_EQ(middle.avg_pq, 999);
}

TEST(DolbyVisionRpu, DerivesLevel1FromStats) {
  platf::hdr_frame_luminance_stats_t stats;
  stats.valid = true;
  stats.avg_maxrgb_pq = 0.5f;        // -> 2048
  stats.avg_maxrgb = 120.0f;
  stats.percentile_99 = 1000.0f;     // -> PQ code 3079
  stats.percentile_10_pq = 0.05f;    // clamps to 12

  const auto metadata = frame_metadata_from_stats(stats);
  ASSERT_TRUE(metadata.has_value());
  EXPECT_EQ(metadata->avg_pq, 2048);
  EXPECT_EQ(metadata->max_pq, 3079);
  EXPECT_EQ(metadata->min_pq, 12);
  EXPECT_FALSE(metadata->scene_refresh);

  // An analyzer that never filled the PQ-domain mean yields nothing, not a
  // zero average: that signature cannot occur on a real frame.
  stats.avg_maxrgb_pq = 0.0f;
  stats.avg_maxrgb = 100.0f;
  EXPECT_FALSE(frame_metadata_from_stats(stats).has_value());

  // A genuinely black frame reports zero in both fields and is kept.
  stats.avg_maxrgb = 0.0f;
  const auto black = frame_metadata_from_stats(stats);
  ASSERT_TRUE(black.has_value());
  EXPECT_EQ(black->avg_pq, video::dolby_vision::l1_avg_pq_min);

  stats.valid = false;
  EXPECT_FALSE(frame_metadata_from_stats(stats).has_value());
}

TEST(DolbyVisionRpu, UsesNearBlackCoverageForARobustMinimum) {
  platf::hdr_frame_luminance_stats_t stats;
  stats.valid = true;
  stats.avg_maxrgb_pq = 0.5f;
  stats.avg_maxrgb = 120.0f;
  stats.percentile_99 = 1000.0f;
  stats.percentile_10_pq = 0.05f;
  stats.percentile_1_pq = 0.001f;  // -> PQ code 4
  stats.near_black_stats_valid = true;

  // A few dark pixels retain the robust first percentile instead of forcing black.
  stats.near_black_fraction = 0.005f;
  auto metadata = frame_metadata_from_stats(stats);
  ASSERT_TRUE(metadata.has_value());
  EXPECT_EQ(metadata->min_pq, 4);

  // A material black region is part of the scene and should be represented as zero.
  stats.near_black_fraction = 0.02f;
  metadata = frame_metadata_from_stats(stats);
  ASSERT_TRUE(metadata.has_value());
  EXPECT_EQ(metadata->min_pq, 0);

  stats.near_black_fraction = std::numeric_limits<float>::quiet_NaN();
  EXPECT_FALSE(frame_metadata_from_stats(stats).has_value());
}

TEST(DolbyVisionRpu, SmoothsAllLevel1FieldsAndResetsAtSceneBoundaries) {
  video::dolby_vision::level1_temporal_filter_t filter;

  frame_metadata_t first { .min_pq = 0, .max_pq = 2081, .avg_pq = 819 };
  EXPECT_EQ(filter.update(first).min_pq, first.min_pq);

  frame_metadata_t next { .min_pq = 12, .max_pq = 4081, .avg_pq = 2819 };
  const auto smoothed = filter.update(next);
  EXPECT_EQ(smoothed.min_pq, 2);
  EXPECT_EQ(smoothed.max_pq, 2381);
  EXPECT_EQ(smoothed.avg_pq, 1119);

  filter.reset();
  const auto reset = filter.update(next);
  EXPECT_EQ(reset.min_pq, next.min_pq);
  EXPECT_EQ(reset.max_pq, next.max_pq);
  EXPECT_EQ(reset.avg_pq, next.avg_pq);
}

TEST(DolbyVisionRpu, EmitsNothingBeforeConfigure) {
  rpu_generator_t generator;
  const auto nal = generator.generate(typical_metadata());
  EXPECT_TRUE(nal.empty());
}

TEST(DolbyVisionRpu, RejectsOutOfRangeConfig) {
  rpu_generator_t generator;

  session_config_t zero_peak;
  zero_peak.source_mastering_peak_nits = 0;
  EXPECT_FALSE(generator.configure(zero_peak));

  session_config_t huge_peak;
  huge_peak.source_mastering_peak_nits = 20001;
  EXPECT_FALSE(generator.configure(huge_peak));

  session_config_t off_frame;
  off_frame.active_area_left = 9000;  // 13-bit field tops out at 8191
  EXPECT_FALSE(generator.configure(off_frame));

  EXPECT_FALSE(generator.configured());
}

TEST(DolbyVisionRpu, WritesDoviHeaderPrefix) {
  rpu_generator_t generator;
  ASSERT_TRUE(generator.configure(session_config_t {}));

  const auto nal = generator.generate(typical_metadata());
  ASSERT_GE(nal.size(), 11u);

  // Header hand-derived from the dovi_tool bit syntax: rpu_type 2,
  // rpu_format 18, profile 1, level 0, seq info present, coefficient data
  // type 0, ue(23), normalized idc 1, full range 0, ue(2)/ue(2)/ue(4), the
  // six single-bit flags, then the mapping's ue(0) sequence.
  const bytes_t expected { 0x19, 0x08, 0x09, 0x08, 0x40, 0x61, 0x36, 0x50, 0x6F };
  const bytes_t actual = unescape(nal.subspan(2));
  EXPECT_GT(actual.size(), expected.size());
  EXPECT_TRUE(std::equal(expected.begin(), expected.end(), actual.begin()));
}

TEST(DolbyVisionRpu, RoundTripsThroughIndependentParser) {
  rpu_generator_t generator;

  session_config_t config;
  config.source_mastering_peak_nits = 1000;
  config.mastering_min_nits_x10000 = 1;
  config.max_cll_nits = 800;
  config.max_fall_nits = 320;
  config.active_area_top = 16;
  config.active_area_bottom = 16;
  ASSERT_TRUE(generator.configure(config));

  const auto metadata = typical_metadata();
  const auto nal = generator.generate(metadata);
  ASSERT_FALSE(nal.empty());

  const auto parsed = parse_nal(nal);
  EXPECT_EQ(parsed.profile, 1);
  EXPECT_EQ(parsed.level, 0);
  EXPECT_EQ(parsed.scene_refresh, 0u);
  EXPECT_EQ(parsed.source_min_pq, 7u);    // PQ floor, 0.0001 nits
  EXPECT_EQ(parsed.source_max_pq, 3079u);  // 1000-nit mastering peak
  EXPECT_EQ(parsed.l1_min, metadata.min_pq);
  EXPECT_EQ(parsed.l1_max, metadata.max_pq);
  EXPECT_EQ(parsed.l1_avg, metadata.avg_pq);
  EXPECT_EQ(parsed.l5[0], 0u);
  EXPECT_EQ(parsed.l5[1], 0u);
  EXPECT_EQ(parsed.l5[2], 16u);
  EXPECT_EQ(parsed.l5[3], 16u);
  EXPECT_EQ(parsed.l6[0], 1000u);
  EXPECT_EQ(parsed.l6[1], 1u);
  EXPECT_EQ(parsed.l6[2], 800u);
  EXPECT_EQ(parsed.l6[3], 320u);

  // 1019 bits of syntax for the scene_refresh=0 template; the byte alignment
  // after num_ext_blocks plus the final alignment land it on 128 metadata
  // bytes + 4 CRC + trailing 0x80.
  const bytes_t rpu = unescape(nal.subspan(2));
  EXPECT_EQ(rpu.size(), 133u);
  EXPECT_EQ(rpu.back(), 0x80);

  // Byte-exact pin: this exact config + metadata was emitted by the reference
  // model and accepted (header, L1/L5/L6, CRC) by dovi_tool 2.3.3
  // `info -s/-f`, which refuses an RPU whose CRC does not match.
  EXPECT_EQ(parsed.crc, 0xdf59b734u);
}

TEST(DolbyVisionRpu, SceneRefreshVariantKeepsTheSameLength) {
  rpu_generator_t generator;
  ASSERT_TRUE(generator.configure(session_config_t {}));

  auto metadata = typical_metadata();
  const bytes_t plain = unescape(generator.generate(metadata).subspan(2));
  const auto parsed_plain = parse_nal(generator.generate(metadata));
  EXPECT_EQ(parsed_plain.scene_refresh, 0u);

  metadata.scene_refresh = true;
  const bytes_t refresh = unescape(generator.generate(metadata).subspan(2));
  const auto parsed_refresh = parse_nal(generator.generate(metadata));
  EXPECT_EQ(parsed_refresh.scene_refresh, 1u);

  // ue(1) takes three bits where ue(0) takes one, but the dm alignment zero
  // padding after num_ext_blocks absorbs the difference: both variants are
  // the same length, and everything downstream of the padding — L1 included —
  // sits at the same offset.
  EXPECT_EQ(plain.size(), 133u);
  EXPECT_EQ(refresh.size(), 133u);
  EXPECT_NE(plain, refresh);
  EXPECT_EQ(parsed_plain.crc_byte_offset, parsed_refresh.crc_byte_offset);
}

TEST(DolbyVisionRpu, GenerateIsDeterministicAndClampsInput) {
  rpu_generator_t generator;
  ASSERT_TRUE(generator.configure(session_config_t {}));

  const auto first = generator.generate({ 5, 4000, 100, false });
  // Copy before the second generate(): both spans point at the same internal
  // buffer, so comparing them in place would compare the buffer with itself.
  const bytes_t first_copy(first.begin(), first.end());
  const auto second = generator.generate({ 5, 4000, 100, false });
  const bytes_t second_copy(second.begin(), second.end());
  EXPECT_EQ(first_copy, second_copy);

  // Out-of-domain input is clamped into the same valid template rather than
  // corrupting the RPU.
  const auto clamped = parse_nal(generator.generate({ 9999, 9999, 9999, false }));
  EXPECT_EQ(clamped.l1_min, video::dolby_vision::l1_min_pq_max);
  EXPECT_EQ(clamped.l1_max, video::dolby_vision::l1_max_pq_max);
  EXPECT_EQ(clamped.l1_avg, video::dolby_vision::l1_max_pq_max - 1);
}

TEST(DolbyVisionRpu, PatchingOnlyMovesLevel1AndCrc) {
  rpu_generator_t generator;
  ASSERT_TRUE(generator.configure(session_config_t {}));

  const auto a = parse_nal(generator.generate({ 1, 2100, 900, false }));
  const auto b = parse_nal(generator.generate({ 12, 4095, 3000, false }));

  EXPECT_NE(a.l1_min, b.l1_min);
  EXPECT_NE(a.l1_max, b.l1_max);
  EXPECT_NE(a.l1_avg, b.l1_avg);
  EXPECT_NE(a.crc, b.crc);
  // Everything else is template constant.
  EXPECT_EQ(a.profile, b.profile);
  EXPECT_EQ(a.source_min_pq, b.source_min_pq);
  EXPECT_EQ(a.source_max_pq, b.source_max_pq);
  EXPECT_EQ(a.crc_byte_offset, b.crc_byte_offset);
}

TEST(DolbyVisionRpu, NalCarriesNoStartCodeEmulation) {
  rpu_generator_t generator;
  ASSERT_TRUE(generator.configure(session_config_t {}));

  // Sweep the domain edges; the L1 patch is the only per-frame variation.
  const frame_metadata_t samples[] = {
    { 0, 2081, 819, false },
    { 12, 4095, 4094, false },
    { 0, 0, 0, true },
    { 12, 4095, 4094, true },
  };
  for (const auto &metadata : samples) {
    const auto nal = generator.generate(metadata);
    ASSERT_GT(nal.size(), 3u);
    for (size_t i = 2; i + 2 < nal.size(); ++i) {
      if (nal[i] == 0x00 && nal[i + 1] == 0x00) {
        EXPECT_GE(nal[i + 2], 0x03u) << "unescaped start-code pattern at " << i;
      }
    }
    // Still parses after the escape sweep.
    parse_nal(nal);
  }
}

TEST(DolbyVisionRpu, StagedQueueBindsRpuToFrameIndex) {
  rpu_generator_t generator;
  ASSERT_TRUE(generator.configure(session_config_t {}));

  staged_rpu_queue_t queue;
  ASSERT_TRUE(queue.stage(10, generator, { 1, 2100, 900, false }));
  ASSERT_TRUE(queue.stage(11, generator, { 2, 2200, 950, true }));
  EXPECT_EQ(queue.size(), 2u);

  const auto eleven = queue.take(11);
  ASSERT_FALSE(eleven.empty());
  const auto parsed_eleven = parse_nal(eleven);
  EXPECT_EQ(parsed_eleven.l1_max, 2200);
  EXPECT_EQ(parsed_eleven.scene_refresh, 1u);

  // take consumes; a second take and an unknown index yield nothing.
  EXPECT_TRUE(queue.take(11).empty());
  EXPECT_TRUE(queue.take(99).empty());

  const auto ten = queue.take(10);
  ASSERT_FALSE(ten.empty());
  EXPECT_EQ(parse_nal(ten).l1_max, 2100);
  EXPECT_EQ(queue.size(), 0u);
}

TEST(DolbyVisionRpu, StagedQueueRefusesOverflowInsteadOfEvicting) {
  rpu_generator_t generator;
  ASSERT_TRUE(generator.configure(session_config_t {}));

  staged_rpu_queue_t queue;
  for (size_t i = 0; i < staged_rpu_queue_t::MAX_IN_FLIGHT; ++i) {
    ASSERT_TRUE(queue.stage(i, generator, typical_metadata()));
  }
  EXPECT_FALSE(queue.stage(staged_rpu_queue_t::MAX_IN_FLIGHT, generator, typical_metadata()));

  // The stranded frame's entry survives the refusal, so its packet can still
  // surface and be matched.
  const auto stranded = queue.take(0);
  EXPECT_FALSE(stranded.empty());

  queue.clear();
  EXPECT_EQ(queue.size(), 0u);
  EXPECT_TRUE(queue.stage(1000, generator, typical_metadata()));
}

namespace {

  using video::dolby_vision::rpu_injector_t;

  /// A minimal access unit with one VCL NAL, a valid injection target.
  bytes_t
  make_au() {
    return bytes_t { 0x00, 0x00, 0x00, 0x01, 0x26, 0x01, 0xAF };  // IDR slice
  }

  platf::hdr_frame_luminance_stats_t
  valid_stats() {
    platf::hdr_frame_luminance_stats_t stats;
    stats.valid = true;
    stats.avg_maxrgb_pq = 0.5f;
    stats.avg_maxrgb = 120.0f;
    stats.percentile_99 = 1000.0f;
    stats.percentile_10_pq = 0.05f;
    return stats;
  }

  size_t
  count_rpu_nals(const bytes_t &au) {
    size_t count = 0;
    for (size_t i = 0; i + 3 < au.size(); ++i) {
      if (au[i] == 0x00 && au[i + 1] == 0x00 && au[i + 2] == 0x01 && au[i + 3] == 0x7C) {
        ++count;
      }
    }
    return count;
  }

  parsed_rpu_t
  parse_injected_rpu(const bytes_t &au) {
    for (size_t i = 0; i + 3 < au.size(); ++i) {
      if (au[i] == 0x00 && au[i + 1] == 0x00 && au[i + 2] == 0x01 && au[i + 3] == 0x7C) {
        return parse_nal(std::span<const uint8_t>(au).subspan(i + 3));
      }
    }
    ADD_FAILURE() << "injected access unit contains no Dolby Vision RPU";
    return {};
  }

}  // namespace

TEST(DolbyVisionInjector, IsInertBeforeConfigure) {
  rpu_injector_t injector;
  EXPECT_FALSE(injector.enabled());

  bytes_t au = make_au();
  injector.stage(1, valid_stats());
  injector.inject(1, au);
  EXPECT_EQ(au, make_au());
}

TEST(DolbyVisionInjector, ConfigureRejectsOutOfRangeConfig) {
  rpu_injector_t injector;
  video::dolby_vision::session_config_t config;
  config.source_mastering_peak_nits = 0;
  EXPECT_FALSE(injector.configure(config));
  EXPECT_FALSE(injector.enabled());
}

TEST(DolbyVisionInjector, ColdAnalyzerShipsWithoutRpu) {
  rpu_injector_t injector;
  ASSERT_TRUE(injector.configure(video::dolby_vision::session_config_t {}));

  platf::hdr_frame_luminance_stats_t cold;
  cold.valid = false;
  injector.stage(1, cold);

  bytes_t au = make_au();
  injector.inject(1, au);
  EXPECT_EQ(au, make_au()) << "no analysis yet means no RPU, like the HDR10+ cold path";
}

TEST(DolbyVisionInjector, BindsRpuToTheEncodedFrameIndex) {
  rpu_injector_t injector;
  ASSERT_TRUE(injector.configure(video::dolby_vision::session_config_t {}));
  injector.stage(5, valid_stats());

  // A different frame's output takes nothing.
  bytes_t other = make_au();
  injector.inject(6, other);
  EXPECT_EQ(other, make_au());

  bytes_t au = make_au();
  injector.inject(5, au);
  EXPECT_EQ(count_rpu_nals(au), 1u);
  EXPECT_GT(au.size(), make_au().size());

  // take() consumed the entry.
  bytes_t again = make_au();
  injector.inject(5, again);
  EXPECT_EQ(again, make_au());
}

TEST(DolbyVisionInjector, ReusesLastAnalysisWhenStatsGoMissing) {
  rpu_injector_t injector;
  ASSERT_TRUE(injector.configure(video::dolby_vision::session_config_t {}));

  injector.stage(1, valid_stats());
  platf::hdr_frame_luminance_stats_t lost;
  lost.valid = false;
  injector.stage(2, lost);

  bytes_t au = make_au();
  injector.inject(2, au);
  EXPECT_EQ(count_rpu_nals(au), 1u) << "conservative metadata beats a metadata-less frame";
}

TEST(DolbyVisionInjector, MarksEachDetectedSceneExactlyOnce) {
  rpu_injector_t injector;
  ASSERT_TRUE(injector.configure(video::dolby_vision::session_config_t {}));

  auto first_scene = valid_stats();
  first_scene.sample_sequence = 1;
  first_scene.percentile_90_pq = 0.60f;
  std::fill(std::begin(first_scene.distribution_maxrgb),
    std::end(first_scene.distribution_maxrgb), 100.0f);

  injector.stage(1, first_scene);
  bytes_t first_au = make_au();
  injector.inject(1, first_au);
  EXPECT_EQ(parse_injected_rpu(first_au).scene_refresh, 1u);

  // Reusing the same analyzer sample must not repeat scene_refresh.
  injector.stage(2, first_scene);
  bytes_t repeated_au = make_au();
  injector.inject(2, repeated_au);
  EXPECT_EQ(parse_injected_rpu(repeated_au).scene_refresh, 0u);

  auto next_scene = first_scene;
  next_scene.sample_sequence = 2;
  std::fill(std::begin(next_scene.distribution_maxrgb),
    std::end(next_scene.distribution_maxrgb), 1000.0f);
  injector.stage(3, next_scene);
  bytes_t next_au = make_au();
  injector.inject(3, next_au);
  EXPECT_EQ(parse_injected_rpu(next_au).scene_refresh, 1u);
}

TEST(DolbyVisionInjector, DefersSceneRefreshUntilMetadataIsValid) {
  rpu_injector_t injector;
  ASSERT_TRUE(injector.configure(video::dolby_vision::session_config_t {}));

  auto first_scene = valid_stats();
  first_scene.sample_sequence = 1;
  first_scene.percentile_90_pq = 0.60f;
  std::fill(std::begin(first_scene.distribution_maxrgb),
    std::end(first_scene.distribution_maxrgb), 100.0f);
  injector.stage(1, first_scene);
  bytes_t first_au = make_au();
  injector.inject(1, first_au);
  ASSERT_EQ(parse_injected_rpu(first_au).scene_refresh, 1u);

  auto invalid_cut = first_scene;
  invalid_cut.sample_sequence = 2;
  invalid_cut.near_black_stats_valid = true;
  invalid_cut.percentile_1_pq = 0.001f;
  invalid_cut.near_black_fraction = std::numeric_limits<float>::quiet_NaN();
  std::fill(std::begin(invalid_cut.distribution_maxrgb),
    std::end(invalid_cut.distribution_maxrgb), 1000.0f);
  injector.stage(2, invalid_cut);
  bytes_t invalid_au = make_au();
  injector.inject(2, invalid_au);
  // The conservative previous metadata is reused without falsely claiming that
  // the new scene's invalid analysis was applied.
  EXPECT_EQ(parse_injected_rpu(invalid_au).scene_refresh, 0u);

  auto recovered = invalid_cut;
  recovered.sample_sequence = 3;
  recovered.near_black_fraction = 0.02f;
  injector.stage(3, recovered);
  bytes_t recovered_au = make_au();
  injector.inject(3, recovered_au);
  EXPECT_EQ(parse_injected_rpu(recovered_au).scene_refresh, 1u);
}

TEST(DolbyVisionInjector, OverflowDisablesTheSession) {
  rpu_injector_t injector;
  ASSERT_TRUE(injector.configure(video::dolby_vision::session_config_t {}));

  const auto stats = valid_stats();
  for (size_t i = 0; i < video::dolby_vision::staged_rpu_queue_t::MAX_IN_FLIGHT; ++i) {
    injector.stage(i, stats);
  }
  EXPECT_TRUE(injector.enabled());

  // One stage beyond capacity: the injector stops rather than risk a stale
  // RPU landing on a newer picture.
  injector.stage(video::dolby_vision::staged_rpu_queue_t::MAX_IN_FLIGHT, stats);
  EXPECT_FALSE(injector.enabled());

  bytes_t au = make_au();
  injector.inject(0, au);
  EXPECT_EQ(au, make_au()) << "queued RPUs are dropped with the session, not flushed";
}
