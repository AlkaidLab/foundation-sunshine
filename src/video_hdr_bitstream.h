/**
 * @file src/video_hdr_bitstream.h
 * @brief Carriage of registered ITU-T T.35 dynamic metadata in HEVC and AV1 bitstreams.
 *
 * NVENC takes a T.35 payload and writes the surrounding syntax itself
 * (NV_ENC_SEI_PAYLOAD / obuPayloadArray). AMF has no equivalent: the encoder
 * component exposes only static AMFHDRMetadata plus AUD/parameter-set insertion,
 * so anything ST 2094-40 or CUVA has to be spliced into the finished bitstream
 * by hand. This is that splice, kept free of AMF, D3D and FFmpeg headers so the
 * byte layout can be unit-tested on any platform.
 *
 * The payload itself is produced by video::hdr_metadata::serialize_hdr10plus_t35()
 * and serialize_vivid_t35(), which already emit a complete registered T.35
 * payload starting at itu_t_t35_country_code. Nothing here inspects it.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace video::hdr_bitstream {

  /**
   * Codecs with a carriage for registered ITU-T T.35 metadata that Sunshine emits.
   *
   * H.264 is deliberately absent. It does define an equivalent SEI, but Sunshine
   * never streams HDR over H.264, so supporting it here would be untested code
   * guarding a state the encoder cannot reach.
   */
  enum class codec_e {
    hevc,  ///< prefix SEI NAL, nal_unit_type 39, payloadType 4
    av1,  ///< OBU_METADATA, metadata_type 4 (METADATA_TYPE_ITUT_T35)
  };

  /**
   * Map the config_t::videoFormat convention (0 H.264, 1 HEVC, 2 AV1) onto a carriage.
   * Returns nullopt for a format this module does not write, which is the caller's
   * signal to skip dynamic metadata entirely.
   */
  std::optional<codec_e>
  codec_for(int video_format);

  /**
   * Wrap one T.35 payload in its codec-specific container and append it to out.
   *
   * out accumulates units so several payloads (HDR10+ and HDR Vivid) can be
   * spliced into an access unit as a single contiguous insert. An empty payload
   * appends nothing and returns false; out is never left holding a partial unit.
   */
  bool
  append_t35_unit(codec_e codec, std::span<const uint8_t> t35, std::vector<uint8_t> &out);

  /**
   * Byte offset in an encoded access unit (HEVC) or temporal unit (AV1) where
   * dynamic metadata has to be spliced, or nullopt when the buffer has no valid
   * insertion point.
   *
   * Both codecs require the metadata to precede the picture data it describes,
   * and both allow it only after the headers: an HEVC prefix SEI belongs after
   * any AUD and parameter sets but before the first VCL NAL, and an AV1 metadata
   * OBU belongs after the temporal delimiter and sequence header but before the
   * frame header. Landing this offset on the first unit that carries picture data
   * satisfies both.
   *
   * nullopt is a normal outcome for buffers the caller should leave alone —
   * a header-only access unit, or an AV1 temporal unit whose OBUs omit
   * obu_size and therefore cannot be walked.
   *
   * For HEVC the offset points at the 0x000001 start code prefix, which for a
   * four-byte start code is one byte past its leading zero. Splicing there can
   * never claim a byte that belonged to the preceding NAL, and the unit written
   * by append_t35_unit() carries its own four-byte start code, so the leftover
   * zero simply becomes a leading zero ahead of it.
   */
  std::optional<size_t>
  insert_offset(codec_e codec, std::span<const uint8_t> bitstream);

  /**
   * Splice units built by append_t35_unit() into an encoded frame.
   *
   * Returns false and leaves bitstream byte-identical when there is nothing to
   * insert or no insertion point exists, so a caller can send the untouched
   * frame rather than dropping it.
   */
  bool
  insert(codec_e codec, std::span<const uint8_t> units, std::vector<uint8_t> &bitstream);

  /**
   * Remove every Dolby Vision RPU NAL (HEVC unspecified NAL type 62) from an
   * access unit, together with its start code.
   *
   * The removal spans from the RPU's own 0x000001 start code to the next one,
   * so it may take a leading zero of a following four-byte start code with it
   * — the remainder is still a conformant three-byte start code, and the
   * matched triple itself is never touched. Returns whether anything was
   * removed.
   */
  bool
  strip_hevc_dolby_vision_rpus(std::vector<uint8_t> &bitstream);

  /**
   * Splice one Dolby Vision RPU into an HEVC access unit as its last NAL.
   *
   * Unlike the T.35 carriage above, which must precede the picture data it
   * describes, an RPU goes at the end of the access unit — the position
   * dovi_tool settled on for player and device compatibility. rpu_nalu is the
   * complete NAL without start code (0x7C 0x01 + emulation-prevented RPU, as
   * produced by video::dolby_vision::rpu_generator_t::generate()); layer 0
   * and temporal id 0 are required, which is what that generator emits.
   *
   * The access unit must carry picture data: injecting into a header-only
   * unit would attach an RPU to no picture at all. Existing RPU NALs are
   * stripped first, so the call is idempotent — running it twice over the
   * same buffer yields identical bytes. Returns false and leaves the
   * bitstream unchanged when rpu_nalu is malformed or the unit has no VCL
   * NAL.
   *
   * Binding the RPU to the right encoded frame is the caller's job; see
   * video::dolby_vision::staged_rpu_queue_t.
   */
  bool
  inject_hevc_dolby_vision_rpu(std::span<const uint8_t> rpu_nalu, std::vector<uint8_t> &bitstream);

}  // namespace video::hdr_bitstream
