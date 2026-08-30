/**
 * @file src/video_dolby_vision.cpp
 * @brief Dolby Vision Profile 8.1 RPU writer.
 */
#include "video_dolby_vision.h"

#include "logging.h"

#include <algorithm>
#include <cmath>

namespace video::dolby_vision {

  namespace {

    /// Upper bound of the unescaped RPU: 0x19 + header + mapping + dm data +
    /// L1/L5/L6 blocks + alignment + CRC + final byte computes to 133 for this
    /// fixed block set; the bound leaves room for a future block.
    constexpr size_t max_rpu_bytes = max_rpu_nal_size;

    /// ETSI TS 103 572 RPU trailer: CRC-32/MPEG-2 (poly 0x04C11DB7, init
    /// 0xFFFFFFFF, no reflection, no final xor) over every byte after the
    /// 0x19 prefix, then this CRC big-endian, then 0x80.
    uint32_t
    crc32_mpeg2(const uint8_t *data, size_t size) {
      uint32_t crc = 0xFFFFFFFFu;
      for (size_t i = 0; i < size; ++i) {
        crc ^= static_cast<uint32_t>(data[i]) << 24;
        for (int bit = 0; bit < 8; ++bit) {
          crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
        }
      }
      return crc;
    }

    /// MSB-first bounded bit writer for template construction. Overflow
    /// freezes the write count so configure() can refuse a too-large template
    /// instead of truncating it.
    class bounded_writer_t {
    public:
      explicit bounded_writer_t(std::array<uint8_t, max_rpu_bytes> &buffer):
          buffer_(buffer) {
      }

      void
      write(uint32_t value, int bit_count) {
        for (int bit = bit_count - 1; bit >= 0 && !overflow_; --bit) {
          const uint32_t masked = (value >> bit) & 1u;
          buffer_[bytes_] |= static_cast<uint8_t>(masked << (7 - bits_));
          if (++bits_ == 8) {
            bits_ = 0;
            if (++bytes_ == buffer_.size()) {
              overflow_ = true;
            }
          }
        }
      }

      /// Map u(v) values (get_bits/write_var in dovi_tool) — plain fixed-width.
      void
      write_var(int bit_count, uint32_t value) {
        write(value, bit_count);
      }

      void
      write_ue(uint64_t value) {
        const uint64_t code = value + 1;
        int leading = 0;
        while ((code >> leading) != 0 && leading < 64) {
          ++leading;
        }
        // code occupies `leading` bits; leading-1 zero bits precede it.
        write(0, leading - 1);
        write(static_cast<uint32_t>(code), leading > 32 ? 32 : leading);
      }

      void
      write_se(int64_t value) {
        const uint64_t mapped = value > 0 ? (2 * static_cast<uint64_t>(value) - 1) : (-2 * static_cast<uint64_t>(value));
        write_ue(mapped);
      }

      /// Zero padding to the next byte boundary (dm_alignment_zero_bit et al.).
      void
      align_zero() {
        while (bits_ != 0 && !overflow_) {
          write(0, 1);
        }
      }

      uint32_t
      bit_position() const {
        return static_cast<uint32_t>(bytes_ * 8 + bits_);
      }

      size_t
      byte_position() const {
        return bytes_ + (bits_ != 0 ? 1 : 0);
      }

      bool
      overflow() const {
        return overflow_;
      }

    private:
      std::array<uint8_t, max_rpu_bytes> &buffer_;
      size_t bytes_ = 0;
      int bits_ = 0;
      bool overflow_ = false;
    };

    /// Set `bit_count` bits of `value` at an MSB-first bit offset.
    void
    patch_bits(uint8_t *buffer, size_t bit_offset, uint16_t value, int bit_count) {
      for (int bit = bit_count - 1; bit >= 0; --bit) {
        const size_t bit_index = bit_offset + static_cast<size_t>(bit_count - 1 - bit);
        const uint8_t mask = static_cast<uint8_t>(1u << (7 - bit_index % 8));
        if ((value >> bit) & 1u) {
          buffer[bit_index / 8] |= mask;
        }
        else {
          buffer[bit_index / 8] &= static_cast<uint8_t>(~mask);
        }
      }
    }

    /// 12-bit PQ code, rounded. lround rather than the truncation pq_to_u12()
    /// applies, because the mastering-display levels must reproduce dovi_tool's
    /// published table (1000 nits -> 3079, 4000 -> 3696, 0.0001 nits -> 7) and
    /// the exact values land just above those integers.
    uint16_t
    pq_code_u12_rounded(float nits) {
      const float pq = hdr_metadata::nits_to_pq(nits);
      return static_cast<uint16_t>(std::lround(std::clamp(pq, 0.0f, 1.0f) * 4095.0f));
    }

    uint16_t
    pq_signal_u12_rounded(float pq) {
      return static_cast<uint16_t>(std::lround(std::clamp(pq, 0.0f, 1.0f) * 4095.0f));
    }

  }  // namespace

  frame_metadata_t
  clamp_level1(uint16_t min_pq, uint16_t max_pq, uint16_t avg_pq) {
    frame_metadata_t result;
    result.min_pq = std::clamp<uint16_t>(min_pq, 0, l1_min_pq_max);
    result.max_pq = std::clamp<uint16_t>(max_pq, l1_max_pq_min, l1_max_pq_max);
    result.avg_pq = std::clamp<uint16_t>(avg_pq, l1_avg_pq_min, static_cast<uint16_t>(result.max_pq - 1));
    return result;
  }

  std::optional<frame_metadata_t>
  frame_metadata_from_stats(const platf::hdr_frame_luminance_stats_t &stats) {
    if (!stats.valid) {
      return std::nullopt;
    }

    const bool finite = std::isfinite(stats.avg_maxrgb_pq) &&
                        std::isfinite(stats.avg_maxrgb) &&
                        std::isfinite(stats.percentile_99) &&
                        std::isfinite(stats.percentile_10_pq) &&
                        (!stats.near_black_stats_valid ||
                          (std::isfinite(stats.percentile_1_pq) &&
                           std::isfinite(stats.near_black_fraction)));
    if (!finite ||
        stats.avg_maxrgb_pq < 0.0f || stats.avg_maxrgb_pq > 1.0f ||
        stats.percentile_99 < 0.0f ||
        stats.percentile_10_pq < 0.0f || stats.percentile_10_pq > 1.0f ||
        (stats.near_black_stats_valid &&
          (stats.percentile_1_pq < 0.0f || stats.percentile_1_pq > 1.0f ||
           stats.near_black_fraction < 0.0f || stats.near_black_fraction > 1.0f))) {
      return std::nullopt;
    }
    // Zero PQ-domain mean beside a positive linear mean means the analyzer
    // never filled the field; a genuinely black frame reports zero in both.
    if (stats.avg_maxrgb_pq == 0.0f && !(stats.avg_maxrgb <= 0.0f)) {
      return std::nullopt;
    }

    frame_metadata_t raw;
    constexpr float meaningful_near_black_coverage = 0.01f;
    const float robust_min_pq = stats.near_black_stats_valid ?
                                  (stats.near_black_fraction >= meaningful_near_black_coverage ?
                                     0.0f : stats.percentile_1_pq) :
                                  stats.percentile_10_pq;
    raw.min_pq = pq_signal_u12_rounded(robust_min_pq);
    raw.max_pq = pq_code_u12_rounded(stats.percentile_99);
    raw.avg_pq = pq_signal_u12_rounded(stats.avg_maxrgb_pq);
    return clamp_level1(raw.min_pq, raw.max_pq, raw.avg_pq);
  }

  frame_metadata_t
  level1_temporal_filter_t::update(const frame_metadata_t &raw) {
    if (!initialized_) {
      min_pq_ = raw.min_pq;
      max_pq_ = raw.max_pq;
      avg_pq_ = raw.avg_pq;
      initialized_ = true;
    }
    else {
      min_pq_ += ALPHA * (raw.min_pq - min_pq_);
      max_pq_ += ALPHA * (raw.max_pq - max_pq_);
      avg_pq_ += ALPHA * (raw.avg_pq - avg_pq_);
    }

    auto filtered = clamp_level1(
      static_cast<uint16_t>(std::lround(min_pq_)),
      static_cast<uint16_t>(std::lround(max_pq_)),
      static_cast<uint16_t>(std::lround(avg_pq_)));
    filtered.scene_refresh = raw.scene_refresh;
    return filtered;
  }

  void
  level1_temporal_filter_t::reset() {
    *this = {};
  }

  bool
  rpu_generator_t::build_template(const session_config_t &config, bool scene_refresh, template_t &out) {
    std::fill(out.bytes.begin(), out.bytes.end(), 0);
    bounded_writer_t writer { out.bytes };

    // Dolby Vision RPU prefix byte.
    writer.write(0x19, 8);

    // rpu_data_header(): dovi_tool RpuDataHeader::p8_default().
    writer.write(2, 6);    // rpu_type
    writer.write(18, 11);  // rpu_format
    writer.write(1, 4);    // vdr_rpu_profile (1 == profiles 4/7/8 family)
    writer.write(0, 4);    // vdr_rpu_level
    writer.write(1, 1);    // vdr_seq_info_present_flag
    writer.write(0, 1);    // chroma_resampling_explicit_filter_flag
    writer.write(0, 2);    // coefficient_data_type
    writer.write_ue(23);   // coefficient_log2_denom
    writer.write(1, 2);    // vdr_rpu_normalized_idc
    writer.write(0, 1);    // bl_video_full_range_flag
    // rpu_format & 0x700 == 0, so the vdr_seq_info extension is present.
    writer.write_ue(2);    // bl_bit_depth_minus8 (10-bit BL)
    writer.write_ue(2);    // el_bit_depth_minus8 (ext_mapping_idc_0_4 = 0)
    writer.write_ue(4);    // vdr_bit_depth_minus8
    writer.write(0, 1);    // spatial_resampling_filter_flag
    writer.write(0, 3);    // reserved_zero_3bits
    writer.write(0, 1);    // el_spatial_resampling_filter_flag
    writer.write(1, 1);    // disable_residual_flag: no EL, and what makes
                           // get_dovi_profile() report 8 rather than 7/4.
    writer.write(1, 1);    // vdr_dm_metadata_present_flag: RPU present
    writer.write(0, 1);    // use_prev_vdr_rpu_flag

    // rpu_data_mapping(): Profile81::rpu_data_mapping(), an identity
    // polynomial per component, so the BL decodes as plain HDR10 without it.
    writer.write_ue(0);  // vdr_rpu_id
    writer.write_ue(0);  // mapping_color_space
    writer.write_ue(0);  // mapping_chroma_format_idc
    for (int component = 0; component < 3; ++component) {
      writer.write_ue(0);                  // num_pivots_minus2: 2 pivots
      writer.write_var(10, 0);             // pivot[0], bl_bit_depth bits
      writer.write_var(10, 1023);          // pivot[1]
    }
    // disable_residual_flag skips the NLQ syntax here.
    writer.write_ue(0);  // num_x_partitions_minus1
    writer.write_ue(0);  // num_y_partitions_minus1
    for (int component = 0; component < 3; ++component) {
      writer.write_ue(0);       // mapping_idc: polynomial
      writer.write_ue(0);       // poly_order_minus1: order 1
      writer.write(0, 1);       // linear_interp_flag
      writer.write_se(0);       // poly_coef_int[0]
      writer.write_var(23, 0);  // poly_coef[0]
      writer.write_se(1);       // poly_coef_int[1]: y = 0 + 1*x
      writer.write_var(23, 0);  // poly_coef[1]
    }

    // vdr_dm_data(): Profile81::dm_data() over VdrDmData::default_pq().
    writer.write_ue(0);                     // affected_dm_metadata_id
    writer.write_ue(0);                     // current_dm_metadata_id
    writer.write_ue(scene_refresh ? 1 : 0);  // scene_refresh_flag
    const int16_t ycc_to_rgb_coef[9] = { 9574, 0, 13802, 9574, -1540, -5348, 9574, 17610, 0 };
    for (const int16_t coef : ycc_to_rgb_coef) {
      writer.write(static_cast<uint16_t>(coef), 16);
    }
    const uint32_t ycc_to_rgb_offset[3] = { 16777216, 134217728, 134217728 };
    for (const uint32_t offset : ycc_to_rgb_offset) {
      writer.write(offset, 32);
    }
    const int16_t rgb_to_lms_coef[9] = { 7222, 8771, 390, 2654, 12430, 1300, 0, 422, 15962 };
    for (const int16_t coef : rgb_to_lms_coef) {
      writer.write(static_cast<uint16_t>(coef), 16);
    }
    writer.write(65535, 16);  // signal_eotf: SMPTE ST 2084
    writer.write(0, 16);      // signal_eotf_param0
    writer.write(0, 16);      // signal_eotf_param1
    writer.write(0, 32);      // signal_eotf_param2
    writer.write(12, 5);      // signal_bit_depth
    writer.write(0, 2);       // signal_color_space: unknown/RGB
    writer.write(0, 2);       // signal_chroma_format: 4:2:0
    writer.write(1, 2);       // signal_full_range_flag... reserved value 1 per default_pq
    writer.write(pq_code_u12_rounded(static_cast<float>(config.mastering_min_nits_x10000) / 10000.0f), 12);  // source_min_pq
    writer.write(pq_code_u12_rounded(static_cast<float>(config.source_mastering_peak_nits)), 12);            // source_max_pq
    writer.write(42, 10);     // source_diagonal

    // CM v2.9 extension blocks: L1 (dynamic), L5 (active area), L6 (HDR10
    // fallback). num_ext_blocks is ue-coded, then the list is byte-aligned.
    writer.write_ue(3);
    writer.align_zero();

    writer.write_ue(5);  // L1 ext_block_length
    writer.write(1, 8);  // block level
    out.l1_bit_offset = writer.bit_position();
    writer.write(0, 12);  // min_pq placeholder
    writer.write(0, 12);  // max_pq placeholder
    writer.write(0, 12);  // avg_pq placeholder
    writer.write(0, 4);   // ext_dm_alignment_zero_bit

    writer.write_ue(7);  // L5 ext_block_length
    writer.write(5, 8);  // block level
    writer.write(config.active_area_left, 13);
    writer.write(config.active_area_right, 13);
    writer.write(config.active_area_top, 13);
    writer.write(config.active_area_bottom, 13);
    writer.write(0, 4);  // ext_dm_alignment_zero_bit

    writer.write_ue(8);  // L6 ext_block_length
    writer.write(6, 8);  // block level
    writer.write(config.source_mastering_peak_nits, 16);  // max_display_mastering_luminance
    writer.write(config.mastering_min_nits_x10000, 16);   // min_display_mastering_luminance
    writer.write(config.max_cll_nits, 16);                // max_content_light_level
    writer.write(config.max_fall_nits, 16);               // max_frame_average_light_level

    writer.align_zero();
    const size_t metadata_bytes = writer.byte_position();
    if (writer.overflow() || metadata_bytes < 2 || metadata_bytes + 5 > out.bytes.size()) {
      out.size = 0;
      return false;
    }

    const uint32_t crc = crc32_mpeg2(out.bytes.data() + 1, metadata_bytes - 1);
    out.bytes[metadata_bytes + 0] = static_cast<uint8_t>(crc >> 24);
    out.bytes[metadata_bytes + 1] = static_cast<uint8_t>(crc >> 16);
    out.bytes[metadata_bytes + 2] = static_cast<uint8_t>(crc >> 8);
    out.bytes[metadata_bytes + 3] = static_cast<uint8_t>(crc);
    out.bytes[metadata_bytes + 4] = 0x80;  // rpu_trailing_byte
    out.size = static_cast<uint16_t>(metadata_bytes + 5);
    return true;
  }

  bool
  rpu_generator_t::configure(const session_config_t &config) {
    reset();

    // Reject rather than clamp: a caller that asks for a 20000-nit mastering
    // peak or an off-frame active area has a bug, and shipping a truncated
    // field would corrupt the RPU silently.
    const uint16_t *active[] = {
      &config.active_area_left,
      &config.active_area_right,
      &config.active_area_top,
      &config.active_area_bottom,
    };
    const bool valid = config.source_mastering_peak_nits >= 1 &&
                       config.source_mastering_peak_nits <= 10000 &&
                       config.mastering_min_nits_x10000 >= 1 &&
                       config.mastering_min_nits_x10000 <= 10000 &&
                       config.max_cll_nits <= 10000 &&
                       config.max_fall_nits <= 10000 &&
                       std::all_of(std::begin(active), std::end(active),
                         [](const uint16_t *offset) { return *offset <= 8191; });
    if (!valid) {
      return false;
    }

    if (!build_template(config, false, plain_) || !build_template(config, true, refresh_)) {
      reset();
      return false;
    }
    configured_ = true;
    return true;
  }

  std::span<const uint8_t>
  rpu_generator_t::generate(const frame_metadata_t &metadata) {
    if (!configured_) {
      return {};
    }

    const template_t &tpl = metadata.scene_refresh && !refresh_.empty() ? refresh_ : plain_;
    if (tpl.empty()) {
      return {};
    }

    std::copy_n(tpl.bytes.begin(), tpl.size, scratch_.begin());
    const auto clamped = clamp_level1(metadata.min_pq, metadata.max_pq, metadata.avg_pq);
    patch_bits(scratch_.data(), tpl.l1_bit_offset, clamped.min_pq, 12);
    patch_bits(scratch_.data(), tpl.l1_bit_offset + 12, clamped.max_pq, 12);
    patch_bits(scratch_.data(), tpl.l1_bit_offset + 24, clamped.avg_pq, 12);

    // The CRC domain is everything after the 0x19 prefix up to the trailer.
    const size_t metadata_bytes = tpl.size - 5;
    const uint32_t crc = crc32_mpeg2(scratch_.data() + 1, metadata_bytes - 1);
    scratch_[metadata_bytes + 0] = static_cast<uint8_t>(crc >> 24);
    scratch_[metadata_bytes + 1] = static_cast<uint8_t>(crc >> 16);
    scratch_[metadata_bytes + 2] = static_cast<uint8_t>(crc >> 8);
    scratch_[metadata_bytes + 3] = static_cast<uint8_t>(crc);

    // NAL header for nal_unit_type 62 (unspecified_nal_unit_type 62):
    // forbidden_zero_bit 0, type 62 << 1 == 0x7C; layer 0, tid+1 == 1.
    nal_[0] = 0x7C;
    nal_[1] = 0x01;
    size_t out = 2;
    int zero_run = 0;
    for (size_t i = 0; i < tpl.size; ++i) {
      const uint8_t byte = scratch_[i];
      if (out + 1 >= nal_.size()) {
        return {};
      }
      if (zero_run == 2 && byte <= 0x03) {
        nal_[out++] = 0x03;
        zero_run = 0;
      }
      nal_[out++] = byte;
      zero_run = (byte == 0x00) ? zero_run + 1 : 0;
    }
    return std::span<const uint8_t>(nal_.data(), out);
  }

  void
  rpu_generator_t::reset() {
    plain_ = {};
    refresh_ = {};
    configured_ = false;
  }

  bool
  staged_rpu_queue_t::stage(uint64_t frame_index, rpu_generator_t &generator, const frame_metadata_t &metadata) {
    const auto nal = generator.generate(metadata);
    if (nal.empty()) {
      return false;
    }

    for (auto &entry : entries_) {
      if (!entry.used) {
        entry.frame_index = frame_index;
        entry.size = static_cast<uint16_t>(nal.size());
        std::copy(nal.begin(), nal.end(), entry.nal.begin());
        entry.used = true;
        return true;
      }
    }
    return false;
  }

  std::span<const uint8_t>
  staged_rpu_queue_t::take(uint64_t frame_index) {
    for (auto &entry : entries_) {
      if (entry.used && entry.frame_index == frame_index) {
        entry.used = false;
        return std::span<const uint8_t>(entry.nal.data(), entry.size);
      }
    }
    return {};
  }

  void
  staged_rpu_queue_t::clear() {
    entries_ = {};
  }

  size_t
  staged_rpu_queue_t::size() const {
    size_t count = 0;
    for (const auto &entry : entries_) {
      if (entry.used) {
        ++count;
      }
    }
    return count;
  }

  bool
  rpu_injector_t::configure(const session_config_t &config) {
    disable();
    if (!generator_.configure(config)) {
      return false;
    }
    enabled_ = true;
    return true;
  }

  void
  rpu_injector_t::stage(uint64_t frame_index, const platf::hdr_frame_luminance_stats_t &stats) {
    if (!enabled_) {
      return;
    }

    // Missing analysis reuses the last good values: once RPUs are flowing,
    // a frame without one would make the client's Dolby engine fall back to
    // static HDR10 mapping for that frame — a visible brightness step.
    if (scene_detector_.observe(stats)) {
      level1_filter_.reset();
      pending_scene_refresh_ = true;
    }
    if (const auto raw_metadata = frame_metadata_from_stats(stats)) {
      auto metadata = level1_filter_.update(*raw_metadata);
      metadata.scene_refresh = pending_scene_refresh_;
      last_metadata_ = metadata;
      // Consume the refresh only after a valid RPU can carry it. An invalid
      // analyzer sample must not strand the new scene without a refresh.
      pending_scene_refresh_ = false;
    }
    if (!last_metadata_) {
      return;  // cold analyzer: this frame ships without an RPU, like HDR10+ does
    }

    const auto metadata_for_frame = *last_metadata_;
    // Reused luminance is intentional, but a refresh belongs to one picture.
    last_metadata_->scene_refresh = false;
    if (!queue_.stage(frame_index, generator_, metadata_for_frame)) {
      // In-flight overflow means the encoder's output can no longer be
      // trusted to surface in order; stop rather than risk a stale RPU
      // landing on a newer picture (docs §3.5).
      BOOST_LOG(error) << "Dolby Vision: in-flight RPU queue exhausted at frame " << frame_index
                       << "; disabling Dolby Vision for this session";
      disable();
    }
  }

  void
  rpu_injector_t::inject(uint64_t frame_index, std::vector<uint8_t> &bitstream) {
    if (!enabled_) {
      return;
    }

    const auto rpu_nalu = queue_.take(frame_index);
    if (rpu_nalu.empty()) {
      return;  // frame was submitted before the analyzer warmed up, or dropped
    }

    if (!hdr_bitstream::inject_hevc_dolby_vision_rpu(rpu_nalu, bitstream)) {
      BOOST_LOG(debug) << "Dolby Vision: no insertion point for frame " << frame_index
                       << "; access unit sent without RPU";
    }
  }

  void
  rpu_injector_t::disable() {
    enabled_ = false;
    scene_detector_.reset();
    level1_filter_.reset();
    last_metadata_.reset();
    pending_scene_refresh_ = false;
    queue_.clear();
    generator_.reset();
  }

}  // namespace video::dolby_vision
