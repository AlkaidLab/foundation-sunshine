/**
 * @file src/video_hdr_bitstream.cpp
 * @brief HEVC prefix SEI and AV1 metadata OBU writers for ITU-T T.35 payloads.
 */
#include "video_hdr_bitstream.h"

namespace video::hdr_bitstream {

  namespace {

    // H.265 table 7-1: nal_unit_type 39 is PREFIX_SEI_NUT. Types 0..31 are the VCL
    // types, which is what makes "first NAL with type <= 31" the picture data.
    constexpr uint8_t hevc_prefix_sei_nut = 39;
    constexpr uint8_t hevc_last_vcl_nut = 31;

    // H.265 D.2.1: user_data_registered_itu_t_t35.
    constexpr uint8_t sei_payload_type_t35 = 4;

    // AV1 6.2.2 (OBU types) and 6.7.1 (metadata types).
    constexpr uint8_t obu_type_frame_header = 3;
    constexpr uint8_t obu_type_tile_group = 4;
    constexpr uint8_t obu_type_metadata = 5;
    constexpr uint8_t obu_type_frame = 6;
    constexpr uint8_t obu_type_redundant_frame_header = 7;
    constexpr uint8_t metadata_type_itut_t35 = 4;

    // H.265 7.4.1.1 / AV1 5.3.4: a byte-aligned RBSP or OBU payload is terminated
    // by a single one bit followed by zero bits.
    constexpr uint8_t trailing_bits_byte = 0x80;

    /**
     * Append an RBSP as a NAL unit payload with emulation prevention bytes.
     *
     * H.265 7.4.2: an 0x03 is inserted whenever two zero bytes would otherwise be
     * followed by a byte in 0x00..0x03, which is what keeps a payload from
     * impersonating a start code prefix. The zero run starts at zero because the
     * second NAL header byte this always follows is nonzero (nuh_temporal_id_plus1
     * is at least 1), so no run can carry over from the header.
     */
    void
    append_escaped_rbsp(std::span<const uint8_t> rbsp, std::vector<uint8_t> &out) {
      int zero_run = 0;
      for (uint8_t byte : rbsp) {
        if (zero_run == 2 && byte <= 0x03) {
          out.push_back(0x03);
          zero_run = 0;
        }
        out.push_back(byte);
        zero_run = (byte == 0x00) ? zero_run + 1 : 0;
      }
    }

    /**
     * Append a value in the ff-byte form H.265 D.2.1 uses for payloadType and
     * payloadSize: as many 0xFF bytes as fit, then the remainder.
     */
    void
    append_ff_coded(size_t value, std::vector<uint8_t> &out) {
      while (value >= 0xFF) {
        out.push_back(0xFF);
        value -= 0xFF;
      }
      out.push_back(static_cast<uint8_t>(value));
    }

    /// AV1 4.10.5 leb128().
    void
    append_leb128(uint64_t value, std::vector<uint8_t> &out) {
      do {
        uint8_t byte = value & 0x7F;
        value >>= 7;
        if (value != 0) {
          byte |= 0x80;
        }
        out.push_back(byte);
      } while (value != 0);
    }

    /**
     * Read a leb128 from offset, advancing it past the value.
     *
     * Returns nullopt on a truncated buffer or a value that runs past the eight
     * bytes AV1 4.10.5 allows, both of which mean the buffer cannot be walked.
     */
    std::optional<uint64_t>
    read_leb128(std::span<const uint8_t> data, size_t &offset) {
      uint64_t value = 0;
      for (int index = 0; index < 8; ++index) {
        if (offset >= data.size()) {
          return std::nullopt;
        }
        const uint8_t byte = data[offset++];
        value |= static_cast<uint64_t>(byte & 0x7F) << (index * 7);
        if ((byte & 0x80) == 0) {
          return value;
        }
      }
      return std::nullopt;
    }

    bool
    append_hevc_prefix_sei(std::span<const uint8_t> t35, std::vector<uint8_t> &out) {
      // Build the RBSP first so payloadSize describes the payload before escaping,
      // which is the length a decoder recovers after it strips the 0x03 bytes.
      std::vector<uint8_t> rbsp;
      rbsp.reserve(t35.size() + 8);
      append_ff_coded(sei_payload_type_t35, rbsp);
      append_ff_coded(t35.size(), rbsp);
      rbsp.insert(rbsp.end(), t35.begin(), t35.end());
      rbsp.push_back(trailing_bits_byte);

      // A four-byte start code, so the boundary stays unambiguous no matter what
      // the byte before the splice point is.
      out.insert(out.end(), { 0x00, 0x00, 0x00, 0x01 });
      out.push_back(static_cast<uint8_t>(hevc_prefix_sei_nut << 1));  // forbidden_zero_bit 0
      out.push_back(0x01);  // nuh_layer_id 0, nuh_temporal_id_plus1 1
      append_escaped_rbsp(rbsp, out);
      return true;
    }

    bool
    append_av1_metadata_obu(std::span<const uint8_t> t35, std::vector<uint8_t> &out) {
      // AV1 5.8.1: metadata_type, then the type's payload, then trailing bits.
      std::vector<uint8_t> payload;
      payload.reserve(t35.size() + 2);
      append_leb128(metadata_type_itut_t35, payload);
      payload.insert(payload.end(), t35.begin(), t35.end());
      payload.push_back(trailing_bits_byte);

      // AV1 5.3.2 obu_header: obu_forbidden_bit 0, obu_type, obu_extension_flag 0,
      // obu_has_size_field 1, obu_reserved_1bit 0. No extension means temporal_id
      // and spatial_id are 0, which matches the single-layer stream Sunshine sends.
      out.push_back(static_cast<uint8_t>((obu_type_metadata << 3) | 0x02));
      append_leb128(payload.size(), out);
      out.insert(out.end(), payload.begin(), payload.end());
      return true;
    }

    /**
     * Offset of the first VCL NAL unit's start code prefix.
     *
     * The offset points at the 0x000001 prefix rather than at any zero byte in
     * front of it. Backing up would risk claiming a byte that belongs to the
     * previous NAL — cabac_zero_words and trailing_zero_8bits both let a NAL end
     * in zeros — and it is not needed: the inserted unit carries its own four-byte
     * start code, so the extra zeros simply become leading zeros ahead of it.
     */
    std::optional<size_t>
    hevc_insert_offset(std::span<const uint8_t> au) {
      for (size_t offset = 0; offset + 3 <= au.size();) {
        if (au[offset] != 0x00 || au[offset + 1] != 0x00 || au[offset + 2] != 0x01) {
          ++offset;
          continue;
        }

        const size_t header = offset + 3;
        if (header >= au.size()) {
          return std::nullopt;
        }
        if (((au[header] >> 1) & 0x3F) <= hevc_last_vcl_nut) {
          return offset;
        }
        offset = header + 1;
      }
      return std::nullopt;
    }

    /// Offset of the first OBU that carries picture data.
    std::optional<size_t>
    av1_insert_offset(std::span<const uint8_t> tu) {
      size_t offset = 0;
      while (offset < tu.size()) {
        const uint8_t header = tu[offset];
        if ((header & 0x80) != 0) {
          return std::nullopt;  // obu_forbidden_bit set: not an OBU stream
        }

        switch ((header >> 3) & 0x0F) {
          case obu_type_frame_header:
          case obu_type_tile_group:
          case obu_type_frame:
          case obu_type_redundant_frame_header:
            return offset;
          default:
            break;
        }

        size_t next = offset + 1 + (((header >> 2) & 0x01) ? 1 : 0);  // obu_extension_flag
        if (((header >> 1) & 0x01) == 0) {  // obu_has_size_field
          // Without obu_size this OBU implicitly runs to the end of the buffer, so
          // there is no way to reach whatever follows it.
          return std::nullopt;
        }
        const auto size = read_leb128(tu, next);
        if (!size || *size > tu.size() - next) {
          return std::nullopt;
        }
        offset = next + static_cast<size_t>(*size);
      }
      return std::nullopt;
    }

    // H.265 table 7-1: unspecified NAL types 0x7C >> 1 == 62 carry the Dolby
    // Vision RPU in Profile 5/7/8 streams.
    constexpr uint8_t hevc_dovi_rpu_nut = 62;

    /**
     * Erase one NAL unit: the run of zeros ahead of its 0x000001 start code,
     * the matched start code itself, and everything through the byte before
     * the next start code's 0x000001 (or the end of the buffer). Returns the
     * erase start, which is where a scanning caller must resume: after the
     * shift, that position holds what used to follow the erased NAL.
     *
     * Walking back over the leading zeros is what makes removal complete: a
     * four-byte start code's first zero sits ahead of the matched triple, and
     * leaving it behind would grow the buffer by one byte per inject/strip
     * cycle. It is also always safe: zeros before a start code can only be
     * the removed NAL's own leading zeros or the previous NAL's
     * trailing_zero_8bits / cabac_zero_words, which a decoder must accept or
     * ignore either way — never meaningful RBSP, since 00 00 inside a payload
     * is escaped.
     */
    size_t
    erase_hevc_nal(std::vector<uint8_t> &au, size_t start_code) {
      size_t begin = start_code;
      while (begin > 0 && au[begin - 1] == 0x00) {
        --begin;
      }

      const size_t payload = start_code + 3;
      size_t next = payload + 1;
      while (next + 3 <= au.size() &&
             !(au[next] == 0x00 && au[next + 1] == 0x00 && au[next + 2] == 0x01)) {
        ++next;
      }
      const size_t end = (next + 3 <= au.size()) ? next : au.size();
      au.erase(au.begin() + static_cast<std::ptrdiff_t>(begin),
        au.begin() + static_cast<std::ptrdiff_t>(end));
      return begin;
    }

    /**
     * Offset of the next 0x000001 start code at or after `from`, or nullopt.
     *
     * The returned offset points at the first 0x00 of the matched triple, so
     * additional zeros in front of it stay attributed to whatever precedes
     * them — the same convention hevc_insert_offset() uses.
     */
    std::optional<size_t>
    find_hevc_start_code(std::span<const uint8_t> au, size_t from) {
      for (size_t offset = from; offset + 3 <= au.size(); ++offset) {
        if (au[offset] == 0x00 && au[offset + 1] == 0x00 && au[offset + 2] == 0x01) {
          return offset;
        }
      }
      return std::nullopt;
    }

  }  // namespace

  std::optional<codec_e>
  codec_for(int video_format) {
    switch (video_format) {
      case 1:
        return codec_e::hevc;
      case 2:
        return codec_e::av1;
      default:
        return std::nullopt;
    }
  }

  bool
  append_t35_unit(codec_e codec, std::span<const uint8_t> t35, std::vector<uint8_t> &out) {
    if (t35.empty()) {
      return false;
    }

    switch (codec) {
      case codec_e::hevc:
        return append_hevc_prefix_sei(t35, out);
      case codec_e::av1:
        return append_av1_metadata_obu(t35, out);
    }
    return false;
  }

  std::optional<size_t>
  insert_offset(codec_e codec, std::span<const uint8_t> bitstream) {
    switch (codec) {
      case codec_e::hevc:
        return hevc_insert_offset(bitstream);
      case codec_e::av1:
        return av1_insert_offset(bitstream);
    }
    return std::nullopt;
  }

  bool
  insert(codec_e codec, std::span<const uint8_t> units, std::vector<uint8_t> &bitstream) {
    if (units.empty()) {
      return false;
    }

    const auto offset = insert_offset(codec, bitstream);
    if (!offset) {
      return false;
    }

    bitstream.insert(
      bitstream.begin() + static_cast<std::ptrdiff_t>(*offset), units.begin(), units.end());
    return true;
  }

  bool
  strip_hevc_dolby_vision_rpus(std::vector<uint8_t> &bitstream) {
    bool removed = false;
    size_t search = 0;
    while (auto start_code = find_hevc_start_code(bitstream, search)) {
      const size_t payload = *start_code + 3;
      if (payload >= bitstream.size()) {
        break;  // start code with no NAL behind it: nothing to attribute
      }
      if (((bitstream[payload] >> 1) & 0x3F) == hevc_dovi_rpu_nut) {
        search = erase_hevc_nal(bitstream, *start_code);  // bytes shifted left; resume at the erase start
        removed = true;
      }
      else {
        search = payload + 1;
      }
    }
    return removed;
  }

  bool
  inject_hevc_dolby_vision_rpu(std::span<const uint8_t> rpu_nalu, std::vector<uint8_t> &bitstream) {
    if (rpu_nalu.size() < 3 ||
        rpu_nalu[0] != (hevc_dovi_rpu_nut << 1) ||  // layer 0, forbidden 0
        rpu_nalu[1] != 0x01) {                      // layer 0, temporal id +1 = 1
      return false;
    }

    // An RPU describes a picture; a header-only access unit has none.
    if (!hevc_insert_offset(bitstream)) {
      return false;
    }

    strip_hevc_dolby_vision_rpus(bitstream);

    bitstream.insert(bitstream.end(), { 0x00, 0x00, 0x00, 0x01 });
    bitstream.insert(bitstream.end(), rpu_nalu.begin(), rpu_nalu.end());
    return true;
  }

}  // namespace video::hdr_bitstream
