/**
 * @file src/video_dolby_vision.h
 * @brief Dolby Vision Profile 8.1 RPU generation for the HEVC HDR10 base layer.
 *
 * Profile 8.1 cross-compatibility mode is one HDR10-compatible HEVC Main10
 * picture plus a Dolby Vision RPU carried in an UNSPEC 62 NAL. Its value over
 * the existing HDR10+ path is not the pixels — both carry the same BT.2020/PQ
 * base layer — but handing tone mapping to the client device's native Dolby
 * Vision engine. See docs/dolby_vision_profile81.md for scope and the staged
 * rollout plan.
 *
 * The writer produces the same bytes libdovi would: RPU prefix 0x19, the
 * p8_default()/Profile81 header fields, an identity (no-op) polynomial
 * mapping so that stripping the RPU leaves a plain HDR10 stream, CM v2.9
 * extension blocks L1/L5/L6, a CRC-32/MPEG-2 trailer, and HEVC emulation
 * prevention. libdovi itself is not linked: the template is constant apart
 * from 36 bits of L1 and the scene refresh flag, so a native writer keeps the
 * streaming path free of a Rust toolchain dependency and of per-frame
 * allocations. dovi_tool remains the offline cross-check (see the unit
 * tests' independent parser).
 */
#pragma once

#include "video_hdr_bitstream.h"
#include "video_hdr_metadata.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace video::dolby_vision {

  /// Valid domains from dovi_tool's ExtMetadataBlockLevel1 (CM v2.9 clamping).
  /// max_pq >= 2081 (~100 nits) and avg_pq >= 819 are what Dolby's own
  /// analysis emits for HDR10-compatible content; anything below would tell a
  /// DV engine the frame is darker than an HDR10 fallback can represent.
  constexpr uint16_t l1_min_pq_max = 12;
  constexpr uint16_t l1_max_pq_min = 2081;
  constexpr uint16_t l1_max_pq_max = 4095;
  constexpr uint16_t l1_avg_pq_min = 819;

  struct frame_metadata_t {
    uint16_t min_pq = 0;
    uint16_t max_pq = 0;
    uint16_t avg_pq = 0;
    bool scene_refresh = false;
  };

  /**
   * Clamp L1 values into the CM v2.9 domain, mirroring dovi_tool's
   * clamp_values_int() ordering: min first, then max, then avg against
   * max_pq - 1. After max_pq is clamped up to 2081 the avg bounds can never
   * cross (819 <= 2080), so a single pass suffices.
   */
  frame_metadata_t
  clamp_level1(uint16_t min_pq, uint16_t max_pq, uint16_t avg_pq);

  /**
   * Derive one frame's L1 from the analyzer statistics.
   *
   * avg comes from avg_maxrgb_pq — the PQ-domain per-pixel mean — never from
   * the linear-light avg_maxrgb: PQ is concave, so PQ(mean(nits)) sits far
   * above mean(PQ(nits)) on dark frames with highlights and would anchor the
   * display's curve too high. max uses the 99th percentile rather than the
   * true peak, for the same scRGB-overshoot reason hdr10plus_from_luminance()
   * reports percentile 99 as maxSCL. min uses the first percentile, but only
   * reports zero when at least one percent of the histogram is in its near-black
   * bin. That keeps a few black UI pixels from pinning the frame minimum while
   * preserving materially black scenes. Older analyzer output falls back to P10.
   *
   * This function is a stateless mapping. The injector applies its dedicated
   * Level 1 temporal filter after conversion so min/avg/max are smoothed as one
   * coherent Dolby metadata tuple.
   *
   * Returns nullopt when the stats are absent or carry the signature of an
   * analyzer that never filled the PQ-domain mean (zero avg_maxrgb_pq beside
   * a positive linear mean) — the same guard vivid_from_stats() applies.
   */
  std::optional<frame_metadata_t>
  frame_metadata_from_stats(const platf::hdr_frame_luminance_stats_t &stats);

  /** Smooth the three Dolby L1 fields together between scene boundaries. */
  class level1_temporal_filter_t {
  public:
    frame_metadata_t
    update(const frame_metadata_t &raw);

    void
    reset();

  private:
    static constexpr float ALPHA = 0.15f;

    float min_pq_ = 0.0f;
    float max_pq_ = 0.0f;
    float avg_pq_ = 0.0f;
    bool initialized_ = false;
  };

  struct session_config_t {
    /// Mastering display peak in nits (L6 max_display_mastering_luminance).
    /// Content metadata only — the client display's peak belongs to
    /// capability negotiation, never to the RPU.
    uint16_t source_mastering_peak_nits = 1000;
    /// L6 min_display_mastering_luminance, in 1/10000-nit units (SS_HDR_METADATA
    /// convention). 1 == 0.0001 nits, the ST 2084 floor.
    uint16_t mastering_min_nits_x10000 = 1;
    uint16_t max_cll_nits = 0;
    uint16_t max_fall_nits = 0;
    /// L5 active-area offsets; zero everywhere means full frame.
    uint16_t active_area_left = 0;
    uint16_t active_area_right = 0;
    uint16_t active_area_top = 0;
    uint16_t active_area_bottom = 0;
  };

  /// Upper bound of the generated NAL (2-byte header + escaped RPU). The
  /// unescaped template is ~132 bytes; escaping adds at most one byte per
  /// three payload bytes.
  constexpr size_t max_rpu_nal_size = 256;

  class rpu_generator_t {
  public:
    /**
     * Build both frame templates (scene_refresh_flag = 0 and 1). The flag is
     * ue(v)-coded — 0 takes one bit, 1 takes three — which shifts everything
     * after it, so each variant carries its own precomputed L1 bit offset.
     * Returns false only on an internal overflow, which cannot happen for
     * this fixed block set; it is there so a future block cannot silently
     * truncate.
     */
    bool
    configure(const session_config_t &config);

    bool
    configured() const {
      return configured_;
    }

    /**
     * Produce the complete UNSPEC 62 NAL (0x7C 0x01 + emulation-prevented
     * RPU, no start code) for one frame. Metadata values are clamped here, so
     * a caller cannot ship an out-of-domain RPU even by accident.
     *
     * The returned span points into this object and stays valid until the
     * next generate() or reset() — the same contract
     * hdr_metadata::dynamic_metadata_builder_t gives its callers.
     */
    std::span<const uint8_t>
    generate(const frame_metadata_t &metadata);

    void
    reset();

  private:
    struct template_t {
      std::array<uint8_t, max_rpu_nal_size> bytes {};
      /// Total RPU bytes including the 4-byte CRC and final 0x80.
      uint16_t size = 0;
      /// Bit offset of the L1 min_pq field (MSB-first numbering).
      uint32_t l1_bit_offset = 0;

      bool
      empty() const {
        return size == 0;
      }
    };

    static bool
    build_template(const session_config_t &config, bool scene_refresh, template_t &out);

    template_t plain_;
    template_t refresh_;
    std::array<uint8_t, max_rpu_nal_size> nal_ {};
    std::array<uint8_t, max_rpu_nal_size> scratch_ {};
    bool configured_ = false;
  };

  /**
   * Fixed-capacity, allocation-free association of encoded frame indices
   * with the RPU generated for that same frame.
   *
   * The encoder pipeline may drop frames, delay output, reorder around an
   * IDR rebuild, or flush its queue on a rate-control reconfiguration, so
   * "Nth capture == Nth encode callback" does not hold. Stage by the frame
   * index that rides through the encoder (NVENC inputTimeStamp/outputTimeStamp
   * round trip, AMF SubmitInput/QueryOutput index), and take() it back when
   * the encoded packet surfaces.
   *
   * Overflow policy follows the plan: stage() fails when no slot is free,
   * and the caller must stop Dolby Vision for the session — never attach a
   * stale RPU to a newer picture, which reads as brightness pumping on scene
   * cuts and flashes. A span returned by take() is valid until that slot is
   * reused by a later stage(); inject it before staging the next frame,
   * which is the natural encode-output loop order anyway.
   */
  class staged_rpu_queue_t {
  public:
    static constexpr size_t MAX_IN_FLIGHT = 64;

    /**
     * Generate and store the RPU for frame_index. Returns false when the
     * queue is full: the oldest entry is deliberately NOT evicted, because
     * that would silently strand a frame whose packet may still surface.
     */
    bool
    stage(uint64_t frame_index, rpu_generator_t &generator, const frame_metadata_t &metadata);

    /// The staged NAL for frame_index, consumed by the call; empty when the
    /// index was never staged (encoder dropped the frame) or already taken.
    std::span<const uint8_t>
    take(uint64_t frame_index);

    void
    clear();

    size_t
    size() const;

  private:
    struct entry_t {
      uint64_t frame_index = 0;
      uint16_t size = 0;
      bool used = false;
      std::array<uint8_t, max_rpu_nal_size> nal {};
    };

    std::array<entry_t, MAX_IN_FLIGHT> entries_ {};
  };

  /**
   * The per-encode-session Dolby Vision state: stages an RPU when a frame is
   * submitted to the encoder and splices it when that frame's access unit
   * surfaces, keyed strictly by frame index (docs §3.5).
   *
   * stage() reuses the last valid analysis when the current stats are absent
   * — conservative metadata beats none once the stream has started carrying
   * RPUs, and frames before the first analysis ship without one, mirroring
   * how the HDR10+ path skips its SEI on a cold analyzer. When the in-flight
   * queue overflows the session disables itself: a stale RPU on a newer
   * picture reads as brightness pumping, which is worse than no RPU.
   */
  class rpu_injector_t {
  public:
    /// Build the frame templates. Returns false — and leaves the injector
    /// disabled — when the config is out of range.
    bool
    configure(const session_config_t &config);

    bool
    enabled() const {
      return enabled_;
    }

    void
    stage(uint64_t frame_index, const platf::hdr_frame_luminance_stats_t &stats);

    /// Splice the staged RPU as the access unit's last NAL. No-op when the
    /// injector is disabled or this frame has nothing staged; a refused
    /// splice (e.g. a header-only access unit) leaves the bitstream intact.
    void
    inject(uint64_t frame_index, std::vector<uint8_t> &bitstream);

    void
    disable();

  private:
    rpu_generator_t generator_;
    staged_rpu_queue_t queue_;
    hdr_metadata::scene_change_detector_t scene_detector_;
    level1_temporal_filter_t level1_filter_;
    std::optional<frame_metadata_t> last_metadata_;
    bool pending_scene_refresh_ = false;
    bool enabled_ = false;
  };

}  // namespace video::dolby_vision
