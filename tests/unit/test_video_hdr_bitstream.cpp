/**
 * @file tests/unit/test_video_hdr_bitstream.cpp
 * @brief Tests for HEVC prefix SEI and AV1 metadata OBU carriage of T.35 payloads.
 */
#include <src/video_hdr_bitstream.h>

#include "../tests_common.h"

namespace {

  using video::hdr_bitstream::append_t35_unit;
  using video::hdr_bitstream::codec_e;
  using video::hdr_bitstream::codec_for;
  using video::hdr_bitstream::insert;
  using video::hdr_bitstream::insert_offset;

  using bytes_t = std::vector<uint8_t>;

  /**
   * Strip emulation prevention bytes the way a decoder does, so the round trip
   * proves the escape is reversible rather than merely plausible.
   */
  bytes_t
  unescape(std::span<const uint8_t> nal_payload) {
    bytes_t out;
    int zero_run = 0;
    for (const uint8_t byte : nal_payload) {
      if (zero_run == 2 && byte == 0x03) {
        zero_run = 0;
        continue;
      }
      out.push_back(byte);
      zero_run = (byte == 0x00) ? zero_run + 1 : 0;
    }
    return out;
  }

}  // namespace

TEST(HdrBitstream, MapsVideoFormatToCarriage) {
  // H.264 defines an equivalent SEI, but Sunshine never streams HDR over it, so
  // there is deliberately no carriage here to reach.
  EXPECT_FALSE(codec_for(0).has_value());
  EXPECT_EQ(codec_for(1), codec_e::hevc);
  EXPECT_EQ(codec_for(2), codec_e::av1);
  EXPECT_FALSE(codec_for(7).has_value());
}

TEST(HdrBitstream, WritesHevcPrefixSei) {
  const bytes_t t35 { 0xB5, 0x00, 0x3C, 0x00, 0x01, 0x04, 0x40 };
  bytes_t unit;
  ASSERT_TRUE(append_t35_unit(codec_e::hevc, t35, unit));

  EXPECT_EQ(unit,
    bytes_t({
      0x00, 0x00, 0x00, 0x01,  // start code prefix
      0x4E, 0x01,  // nal_unit_type 39 (PREFIX_SEI_NUT), nuh_temporal_id_plus1 1
      0x04,  // payloadType 4 (user_data_registered_itu_t_t35)
      0x07,  // payloadSize
      0xB5, 0x00, 0x3C, 0x00, 0x01, 0x04, 0x40,
      0x80,  // rbsp_trailing_bits
    }));
}

TEST(HdrBitstream, EscapesStartCodePatternsInHevcPayload) {
  // A payload holding both 00 00 01 and a trailing 00 00, i.e. exactly the patterns
  // that would otherwise read as a start code prefix (H.265 7.4.2).
  const bytes_t t35 { 0xB5, 0x00, 0x00, 0x01, 0x00, 0x00, 0x02, 0x00, 0x00 };
  bytes_t unit;
  ASSERT_TRUE(append_t35_unit(codec_e::hevc, t35, unit));

  EXPECT_EQ(unit,
    bytes_t({
      0x00, 0x00, 0x00, 0x01, 0x4E, 0x01,
      0x04, 0x09,
      0xB5, 0x00, 0x00, 0x03, 0x01, 0x00, 0x00, 0x03, 0x02, 0x00, 0x00,
      0x80,
    }));

  // payloadSize describes the payload as a decoder recovers it, after the 0x03
  // bytes are stripped — not the escaped length on the wire.
  const auto rbsp = unescape(std::span(unit).subspan(6));
  ASSERT_EQ(rbsp.size(), 2 + t35.size() + 1);
  EXPECT_EQ(rbsp[0], 0x04);
  EXPECT_EQ(rbsp[1], t35.size());
  EXPECT_TRUE(std::equal(t35.begin(), t35.end(), rbsp.begin() + 2));
  EXPECT_EQ(rbsp.back(), 0x80);
}

TEST(HdrBitstream, WritesHevcPayloadSizeAsFfBytes) {
  bytes_t unit;
  ASSERT_TRUE(append_t35_unit(codec_e::hevc, bytes_t(300, 0xAA), unit));
  EXPECT_EQ(bytes_t(unit.begin() + 6, unit.begin() + 9), bytes_t({ 0x04, 0xFF, 0x2D }));
  // 0xAA never triggers escaping, so the length is exact.
  EXPECT_EQ(unit.size(), 4 + 2 + 1 + 2 + 300 + 1);

  // 255 needs a continuation byte plus a zero remainder, not a bare 0xFF.
  unit.clear();
  ASSERT_TRUE(append_t35_unit(codec_e::hevc, bytes_t(255, 0xAA), unit));
  EXPECT_EQ(bytes_t(unit.begin() + 6, unit.begin() + 9), bytes_t({ 0x04, 0xFF, 0x00 }));
}

TEST(HdrBitstream, WritesAv1MetadataObu) {
  const bytes_t t35 { 0xB5, 0x00, 0x3C, 0x00, 0x01, 0x04 };
  bytes_t unit;
  ASSERT_TRUE(append_t35_unit(codec_e::av1, t35, unit));

  EXPECT_EQ(unit,
    bytes_t({
      0x2A,  // OBU_METADATA (5), obu_has_size_field 1
      0x08,  // obu_size: metadata_type + 6 payload bytes + trailing bits
      0x04,  // metadata_type METADATA_TYPE_ITUT_T35
      0xB5, 0x00, 0x3C, 0x00, 0x01, 0x04,
      0x80,  // trailing bits
    }));

  // obu_size crossing the one-byte leb128 boundary.
  unit.clear();
  ASSERT_TRUE(append_t35_unit(codec_e::av1, bytes_t(200, 0xAA), unit));
  EXPECT_EQ(bytes_t(unit.begin(), unit.begin() + 3), bytes_t({ 0x2A, 0xCA, 0x01 }));
  EXPECT_EQ(unit.size(), 1 + 2 + 202);
}

TEST(HdrBitstream, RejectsEmptyPayloadWithoutTouchingOutput) {
  bytes_t unit { 0xFF };
  EXPECT_FALSE(append_t35_unit(codec_e::hevc, {}, unit));
  EXPECT_FALSE(append_t35_unit(codec_e::av1, {}, unit));
  EXPECT_EQ(unit, bytes_t({ 0xFF }));
}

TEST(HdrBitstream, SplicesHevcUnitBeforeTheFirstVclNal) {
  // VPS(32), SPS(33), PPS(34), prefix SEI(39), then an IDR_W_RADL slice(19).
  const bytes_t au {
    0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0x0C,  // VPS
    0x00, 0x00, 0x00, 0x01, 0x42, 0x01, 0x01,  // SPS
    0x00, 0x00, 0x00, 0x01, 0x44, 0x01, 0xC0,  // PPS
    0x00, 0x00, 0x00, 0x01, 0x4E, 0x01, 0x04,  // prefix SEI
    0x00, 0x00, 0x00, 0x01, 0x26, 0x01, 0xAF,  // IDR_W_RADL slice
  };

  // Byte 28 is the leading zero of the slice's four-byte start code; the offset
  // deliberately points one past it, at the 00 00 01 prefix, so the splice can never
  // claim a byte that belongs to the previous NAL (which may legally end in zeros).
  const auto offset = insert_offset(codec_e::hevc, au);
  ASSERT_TRUE(offset.has_value());
  EXPECT_EQ(*offset, 29U);

  bytes_t frame = au;
  const bytes_t unit { 0xDE, 0xAD };
  ASSERT_TRUE(insert(codec_e::hevc, unit, frame));
  ASSERT_EQ(frame.size(), au.size() + unit.size());
  EXPECT_EQ(frame[29], 0xDE);
  EXPECT_EQ(frame[30], 0xAD);
  EXPECT_TRUE(std::equal(au.begin(), au.begin() + 29, frame.begin())) << "headers changed";
  EXPECT_TRUE(std::equal(au.begin() + 29, au.end(), frame.begin() + 31)) << "slice changed";
}

TEST(HdrBitstream, FindsHevcOffsetWithThreeByteStartCodes) {
  const bytes_t au {
    0x00, 0x00, 0x01, 0x4E, 0x01, 0x04,  // prefix SEI
    0x00, 0x00, 0x01, 0x02, 0x01, 0xAF,  // TRAIL_R slice (nal_unit_type 1)
  };
  const auto offset = insert_offset(codec_e::hevc, au);
  ASSERT_TRUE(offset.has_value());
  EXPECT_EQ(*offset, 6U);
}

TEST(HdrBitstream, HevcScanIsDeterministicAboutEmbeddedStartCodes) {
  // A NAL body holding an unescaped 00 00 01 26 reads as a VCL NAL, which is
  // precisely why real encoders escape it. Pin the behavior rather than leave it
  // to chance.
  const bytes_t au {
    0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0x00, 0x00, 0x01, 0x26,
    0x00, 0x00, 0x00, 0x01, 0x02, 0x01, 0xAF,
  };
  const auto offset = insert_offset(codec_e::hevc, au);
  ASSERT_TRUE(offset.has_value());
  EXPECT_EQ(*offset, 6U);
}

TEST(HdrBitstream, LeavesHevcBufferUntouchedWhenThereIsNoInsertionPoint) {
  const bytes_t headers_only {
    0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0x0C,
    0x00, 0x00, 0x00, 0x01, 0x42, 0x01, 0x01,
  };
  EXPECT_FALSE(insert_offset(codec_e::hevc, headers_only).has_value());

  bytes_t frame = headers_only;
  EXPECT_FALSE(insert(codec_e::hevc, bytes_t({ 0xDE, 0xAD }), frame));
  EXPECT_EQ(frame, headers_only) << "a failed splice must leave the frame sendable";

  EXPECT_FALSE(insert_offset(codec_e::hevc, {}).has_value());
  EXPECT_FALSE(insert_offset(codec_e::hevc, bytes_t({ 0x00, 0x00, 0x01 })).has_value());
}

TEST(HdrBitstream, SplicesAv1UnitBeforeTheFirstPictureDataObu) {
  // OBU_TEMPORAL_DELIMITER(2, empty), OBU_SEQUENCE_HEADER(1), OBU_FRAME(6).
  const bytes_t tu {
    0x12, 0x00,  // temporal delimiter, obu_size 0
    0x0A, 0x03, 0x00, 0x00, 0x00,  // sequence header, obu_size 3
    0x32, 0x02, 0xAA, 0xBB,  // frame OBU, obu_size 2
  };
  const auto offset = insert_offset(codec_e::av1, tu);
  ASSERT_TRUE(offset.has_value());
  EXPECT_EQ(*offset, 7U);

  bytes_t frame = tu;
  const bytes_t unit { 0xDE, 0xAD };
  ASSERT_TRUE(insert(codec_e::av1, unit, frame));
  ASSERT_EQ(frame.size(), tu.size() + unit.size());
  EXPECT_EQ(frame[7], 0xDE);
  EXPECT_EQ(frame[8], 0xAD);
  EXPECT_TRUE(std::equal(tu.begin(), tu.begin() + 7, frame.begin()));
  EXPECT_TRUE(std::equal(tu.begin() + 7, tu.end(), frame.begin() + 9));
}

TEST(HdrBitstream, AccountsForAv1ExtensionHeaderByte) {
  const bytes_t tu {
    0x12, 0x00,  // temporal delimiter
    0x0E, 0x00, 0x02, 0x00, 0x00,  // sequence header with obu_extension_flag set
    0x1A, 0x01, 0xAA,  // OBU_FRAME_HEADER
    0x22, 0x01, 0xBB,  // OBU_TILE_GROUP
  };
  const auto offset = insert_offset(codec_e::av1, tu);
  ASSERT_TRUE(offset.has_value());
  EXPECT_EQ(*offset, 7U);
}

TEST(HdrBitstream, BailsOutOfAv1TemporalUnitsItCannotWalk) {
  // Without obu_size the OBU implicitly runs to the end of the buffer, so whatever
  // follows it can never be located.
  EXPECT_FALSE(insert_offset(codec_e::av1, bytes_t({ 0x12, 0x00, 0x08, 0xAA, 0xBB })).has_value());
  // obu_forbidden_bit set: not an OBU stream at all.
  EXPECT_FALSE(insert_offset(codec_e::av1, bytes_t({ 0x92, 0x00 })).has_value());
  // obu_size 64 with a single byte left.
  EXPECT_FALSE(insert_offset(codec_e::av1, bytes_t({ 0x0A, 0x40, 0x00 })).has_value());
  // Headers only, no picture data.
  EXPECT_FALSE(insert_offset(codec_e::av1, bytes_t({ 0x12, 0x00, 0x0A, 0x01, 0x00 })).has_value());
}

TEST(HdrBitstream, SplicesSeveralUnitsAsOneContiguousInsert) {
  // HDR10+ and HDR Vivid share one access unit; both must land ahead of the slice
  // and in the order they were appended.
  bytes_t units;
  ASSERT_TRUE(append_t35_unit(codec_e::hevc, bytes_t({ 0xB5, 0x01 }), units));
  ASSERT_TRUE(append_t35_unit(codec_e::hevc, bytes_t({ 0x26, 0x02 }), units));

  bytes_t frame { 0x00, 0x00, 0x00, 0x01, 0x26, 0x01, 0xAF };
  ASSERT_TRUE(insert(codec_e::hevc, units, frame));
  ASSERT_EQ(frame.size(), 7 + units.size());
  EXPECT_TRUE(std::equal(units.begin(), units.end(), frame.begin() + 1));
  EXPECT_EQ(frame.back(), 0xAF);
}

namespace {

  using video::hdr_bitstream::inject_hevc_dolby_vision_rpu;
  using video::hdr_bitstream::strip_hevc_dolby_vision_rpus;

  /// A plausible RPU NAL: type 62 header plus a body without start-code patterns.
  bytes_t
  make_rpu_nal(bytes_t body = { 0x19, 0x08, 0x09, 0xAA, 0xBB }) {
    bytes_t nal { 0x7C, 0x01 };
    nal.insert(nal.end(), body.begin(), body.end());
    return nal;
  }

  /// A start-code-prefixed NAL of the given type.
  bytes_t
  make_nal(uint8_t type, bytes_t body = { 0x01, 0xAA }) {
    bytes_t nal { 0x00, 0x00, 0x00, 0x01 };
    nal.push_back(static_cast<uint8_t>(type << 1));
    nal.push_back(0x01);
    nal.insert(nal.end(), body.begin(), body.end());
    return nal;
  }

  size_t
  count_rpu_nals(const bytes_t &au) {
    size_t count = 0;
    for (size_t offset = 0; offset + 3 < au.size(); ++offset) {
      if (au[offset] == 0x00 && au[offset + 1] == 0x00 && au[offset + 2] == 0x01 &&
          ((au[offset + 3] >> 1) & 0x3F) == 62) {
        ++count;
      }
    }
    return count;
  }

  bool
  ends_with(const bytes_t &au, const bytes_t &suffix) {
    return au.size() >= suffix.size() &&
           std::equal(suffix.begin(), suffix.end(), au.end() - static_cast<std::ptrdiff_t>(suffix.size()));
  }

  bytes_t
  concat(std::initializer_list<bytes_t> parts) {
    bytes_t out;
    for (const auto &part : parts) {
      out.insert(out.end(), part.begin(), part.end());
    }
    return out;
  }

}  // namespace

TEST(HdrBitstream, InjectsDolbyVisionRpuAsTheLastNal) {
  // VPS, SPS, PPS, then an IDR slice — the RPU goes after all of them.
  const bytes_t au = concat({
    make_nal(32),  // VPS
    make_nal(33),  // SPS
    make_nal(34),  // PPS
    make_nal(19),  // IDR_W_RADL slice
  });

  const bytes_t rpu = make_rpu_nal();
  bytes_t frame = au;
  ASSERT_TRUE(inject_hevc_dolby_vision_rpu(rpu, frame));

  ASSERT_EQ(frame.size(), au.size() + 4 + rpu.size());
  EXPECT_EQ(count_rpu_nals(frame), 1u);
  const bytes_t tail = concat({ { 0x00, 0x00, 0x00, 0x01 }, rpu });
  EXPECT_TRUE(ends_with(frame, tail));
  // The original access unit is untouched ahead of the appended NAL.
  EXPECT_TRUE(std::equal(au.begin(), au.end(), frame.begin()));
}

TEST(HdrBitstream, RpuInjectionIsIdempotent) {
  const bytes_t au = concat({ make_nal(33), make_nal(1) });

  const bytes_t rpu = make_rpu_nal();
  bytes_t once = au;
  ASSERT_TRUE(inject_hevc_dolby_vision_rpu(rpu, once));

  bytes_t twice = once;
  ASSERT_TRUE(inject_hevc_dolby_vision_rpu(rpu, twice));
  EXPECT_EQ(twice, once) << "re-running the injector must not duplicate the RPU";
  EXPECT_EQ(count_rpu_nals(twice), 1u);
}

TEST(HdrBitstream, InjectionReplacesAStaleRpu) {
  // An access unit that already carries an RPU mid-stream (e.g. an encoder
  // that wrote one, or a retried frame) must end up with exactly the new RPU.
  const bytes_t rpu_old = make_rpu_nal({ 0x19, 0x08, 0x11, 0x22 });
  const bytes_t rpu_new = make_rpu_nal({ 0x19, 0x08, 0x33, 0x44, 0x55 });

  const bytes_t au = concat({
    make_nal(33),                                       // SPS
    concat({ { 0x00, 0x00, 0x00, 0x01 }, rpu_old }),  // stale RPU
    make_nal(1),                                        // slice
  });
  ASSERT_EQ(count_rpu_nals(au), 1u);

  bytes_t frame = au;
  ASSERT_TRUE(inject_hevc_dolby_vision_rpu(rpu_new, frame));

  EXPECT_EQ(count_rpu_nals(frame), 1u);
  const bytes_t tail = concat({ { 0x00, 0x00, 0x00, 0x01 }, rpu_new });
  EXPECT_TRUE(ends_with(frame, tail));
  // The stale body bytes are gone, not merely superseded by position.
  EXPECT_EQ(std::search(frame.begin(), frame.end(), rpu_old.begin(), rpu_old.end()), frame.end());
}

TEST(HdrBitstream, InjectsOneRpuIntoAMultiSlicePicture) {
  const bytes_t au = concat({ make_nal(1, { 0x01, 0xA1 }), make_nal(1, { 0x01, 0xA2 }) });

  const bytes_t rpu = make_rpu_nal();
  bytes_t frame = au;
  ASSERT_TRUE(inject_hevc_dolby_vision_rpu(rpu, frame));
  EXPECT_EQ(count_rpu_nals(frame), 1u);
  EXPECT_TRUE(std::equal(au.begin(), au.end(), frame.begin()));
  EXPECT_EQ(frame.size(), au.size() + 4 + rpu.size());
}

TEST(HdrBitstream, RejectsRpuInjectionIntoHeaderOnlyAccessUnit) {
  const bytes_t headers_only = concat({ make_nal(32), make_nal(33) });

  bytes_t frame = headers_only;
  EXPECT_FALSE(inject_hevc_dolby_vision_rpu(make_rpu_nal(), frame));
  EXPECT_EQ(frame, headers_only) << "a refused injection must leave the frame sendable";

  // Malformed RPU references are refused before anything is touched.
  EXPECT_FALSE(inject_hevc_dolby_vision_rpu({}, frame));
  EXPECT_FALSE(inject_hevc_dolby_vision_rpu(bytes_t({ 0x7C }), frame));
  EXPECT_FALSE(inject_hevc_dolby_vision_rpu(bytes_t({ 0x78, 0x01, 0xAA }), frame));  // type 60
  EXPECT_FALSE(inject_hevc_dolby_vision_rpu(bytes_t({ 0x7C, 0x02, 0xAA }), frame));  // temporal id 1
  EXPECT_EQ(frame, headers_only);
}

TEST(HdrBitstream, StripsDolbyVisionRpus) {
  const bytes_t rpu = make_rpu_nal({ 0x19, 0x08, 0x77 });
  const bytes_t au = concat({
    make_nal(33),
    concat({ { 0x00, 0x00, 0x00, 0x01 }, rpu }),
    make_nal(1),
  });
  ASSERT_EQ(count_rpu_nals(au), 1u);

  bytes_t frame = au;
  EXPECT_TRUE(strip_hevc_dolby_vision_rpus(frame));
  EXPECT_EQ(count_rpu_nals(frame), 0u);
  // The SPS survives untouched and the slice keeps a valid start code: the
  // erase takes the stale RPU's start code with the zeros ahead of it, so the
  // slice's four-byte code loses its leading zero to the walk-back and reads
  // as a three-byte code — still conformant Annex-B.
  EXPECT_EQ(frame,
    bytes_t({
      0x00, 0x00, 0x00, 0x01, 0x42, 0x01, 0x01, 0xAA,  // SPS
      0x00, 0x00, 0x01, 0x02, 0x01, 0x01, 0xAA,        // slice, three-byte start code
    }));

  // Stripping is idempotent.
  EXPECT_FALSE(strip_hevc_dolby_vision_rpus(frame));
  EXPECT_EQ(count_rpu_nals(frame), 0u);
}
