/**
 * @file src/video_hdr_metadata.h
 * @brief Helpers for generating and stabilizing HDR dynamic metadata.
 */
#pragma once

#include "platform/common.h"
#include "video_colorspace.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace video::hdr_metadata {

  struct formats_t {
    bool hdr10plus = false;
    bool vivid = false;
  };

  /**
   * HDR10+ is defined for PQ content. HDR Vivid can accompany either PQ or HLG.
   */
  inline formats_t
  formats_for(const sunshine_colorspace_t &colorspace) {
    switch (colorspace.colorspace) {
      case colorspace_e::bt2020:
        return { .hdr10plus = true, .vivid = true };
      case colorspace_e::bt2020hlg:
        return { .hdr10plus = false, .vivid = true };
      default:
        return {};
    }
  }

  /**
   * Convert absolute display luminance to the normalized SMPTE ST 2084 signal
   * used by GB/T 46269.1-2025 (equivalent to T/UWA 005.1-2024).
   */
  inline float
  nits_to_pq(float nits) {
    constexpr double m1 = 2610.0 / 4096.0 / 4.0;
    constexpr double m2 = 2523.0 / 4096.0 * 128.0;
    constexpr double c1 = 3424.0 / 4096.0;
    constexpr double c2 = 2413.0 / 4096.0 * 32.0;
    constexpr double c3 = 2392.0 / 4096.0 * 32.0;

    if (!std::isfinite(nits)) {
      return 0.0f;
    }

    const double normalized = std::clamp(static_cast<double>(nits) / 10000.0, 0.0, 1.0);
    const double powered = std::pow(normalized, m1);
    return static_cast<float>(std::pow((c1 + c2 * powered) / (1.0 + c3 * powered), m2));
  }

  inline float
  pq_to_nits(float pq) {
    constexpr double m1 = 2610.0 / 4096.0 / 4.0;
    constexpr double m2 = 2523.0 / 4096.0 * 128.0;
    constexpr double c1 = 3424.0 / 4096.0;
    constexpr double c2 = 2413.0 / 4096.0 * 32.0;
    constexpr double c3 = 2392.0 / 4096.0 * 32.0;

    if (!std::isfinite(pq)) {
      return 0.0f;
    }

    const double powered =
      std::pow(std::clamp(static_cast<double>(pq), 0.0, 1.0), 1.0 / m2);
    const double numerator = std::max(powered - c1, 0.0);
    const double denominator = std::max(c2 - c3 * powered, 1.0e-12);
    return static_cast<float>(10000.0 * std::pow(numerator / denominator, 1.0 / m1));
  }

  inline uint16_t
  pq_to_u12(float pq) {
    if (!std::isfinite(pq)) {
      return 0;
    }
    return static_cast<uint16_t>(std::clamp(pq, 0.0f, 1.0f) * 4095.0f);
  }

  struct vivid_metadata_t {
    uint16_t minimum_maxrgb_pq = 0;
    uint16_t average_maxrgb_pq = 0;
    uint16_t variance_maxrgb_pq = 0;
    uint16_t maximum_maxrgb_pq = 0;
    bool valid = false;
  };

  /**
   * Generate the four statistics-mode HDR Vivid fields from content values.
   *
   * The fields describe the content in the PQ signal domain. Target display
   * luminance is intentionally not an input: it belongs to display adaptation
   * and optional curve parameters, not to these content statistics.
   */
  inline vivid_metadata_t
  vivid_from_stats(const platf::hdr_frame_luminance_stats_t &stats) {
    if (!stats.valid) {
      return {};
    }

    vivid_metadata_t result;
    result.minimum_maxrgb_pq = pq_to_u12(nits_to_pq(stats.min_maxrgb));
    result.average_maxrgb_pq = pq_to_u12(nits_to_pq(stats.avg_maxrgb));
    result.variance_maxrgb_pq = pq_to_u12(
      std::max(stats.percentile_90_pq - stats.percentile_10_pq, 0.0f));
    result.maximum_maxrgb_pq = pq_to_u12(nits_to_pq(stats.max_maxrgb));
    result.valid = true;
    return result;
  }

  /**
   * GB/T 46269.1-2025 Annex A.9 recommends a 32-frame arithmetic mean over
   * generated dynamic metadata. Reused analyzer samples are deliberately added
   * once per encoded frame so the window remains 32 video frames even when GPU
   * analysis runs at a lower cadence. reset() is available for a future reliable
   * scene-cut signal; this layer deliberately does not guess one from brightness.
   */
  class vivid_temporal_filter_t {
  public:
    vivid_metadata_t
    update(const platf::hdr_frame_luminance_stats_t &stats) {
      return update(vivid_from_stats(stats));
    }

    vivid_metadata_t
    update(const vivid_metadata_t &metadata) {
      if (!metadata.valid) {
        return {};
      }

      if (count_ == WINDOW_SIZE) {
        subtract(samples_[next_]);
      }
      else {
        ++count_;
      }

      samples_[next_] = metadata;
      add(metadata);
      next_ = (next_ + 1) % WINDOW_SIZE;

      vivid_metadata_t result;
      result.minimum_maxrgb_pq = static_cast<uint16_t>(minimum_sum_ / count_);
      result.average_maxrgb_pq = static_cast<uint16_t>(average_sum_ / count_);
      result.variance_maxrgb_pq = static_cast<uint16_t>(variance_sum_ / count_);
      result.maximum_maxrgb_pq = static_cast<uint16_t>(maximum_sum_ / count_);
      result.valid = true;
      return result;
    }

    void
    reset() {
      *this = {};
    }

  private:
    static constexpr size_t WINDOW_SIZE = 32;

    void
    add(const vivid_metadata_t &metadata) {
      minimum_sum_ += metadata.minimum_maxrgb_pq;
      average_sum_ += metadata.average_maxrgb_pq;
      variance_sum_ += metadata.variance_maxrgb_pq;
      maximum_sum_ += metadata.maximum_maxrgb_pq;
    }

    void
    subtract(const vivid_metadata_t &metadata) {
      minimum_sum_ -= metadata.minimum_maxrgb_pq;
      average_sum_ -= metadata.average_maxrgb_pq;
      variance_sum_ -= metadata.variance_maxrgb_pq;
      maximum_sum_ -= metadata.maximum_maxrgb_pq;
    }

    std::array<vivid_metadata_t, WINDOW_SIZE> samples_ {};
    size_t next_ = 0;
    uint32_t count_ = 0;
    uint32_t minimum_sum_ = 0;
    uint32_t average_sum_ = 0;
    uint32_t variance_sum_ = 0;
    uint32_t maximum_sum_ = 0;
  };

  namespace detail {

    class bit_writer_t {
    public:
      explicit bit_writer_t(std::vector<uint8_t> &buffer):
          buffer_(buffer) {
      }

      void
      write(uint32_t value, int bit_count) {
        for (int bit = bit_count - 1; bit >= 0; --bit) {
          accumulator_ = (accumulator_ << 1) | ((value >> bit) & 1U);
          ++pending_bits_;
          if (pending_bits_ == 8) {
            buffer_.push_back(static_cast<uint8_t>(accumulator_));
            accumulator_ = 0;
            pending_bits_ = 0;
          }
        }
      }

      void
      flush() {
        if (pending_bits_ == 0) {
          return;
        }

        accumulator_ <<= 8 - pending_bits_;
        buffer_.push_back(static_cast<uint8_t>(accumulator_));
        accumulator_ = 0;
        pending_bits_ = 0;
      }

    private:
      std::vector<uint8_t> &buffer_;
      uint32_t accumulator_ = 0;
      int pending_bits_ = 0;
    };

  }  // namespace detail

  /**
   * Serialize a CUVA HDR Vivid ITU-T T.35 payload.
   *
   * For system_start_code 0x01, T/UWA 005.1 fixes num_windows to one; it is not
   * present in the bitstream. tone_mapping_param_num is likewise absent when
   * tone_mapping_mode_flag is zero.
   */
  inline size_t
  serialize_vivid_t35(const vivid_metadata_t &metadata, std::vector<uint8_t> &payload) {
    if (!metadata.valid) {
      payload.clear();
      return 0;
    }

    payload.clear();
    payload.reserve(16);
    payload.push_back(0x26);  // itu_t_t35_country_code: China
    payload.push_back(0x00);
    payload.push_back(0x04);  // itu_t_t35_terminal_provider_code: CUVA
    payload.push_back(0x00);
    payload.push_back(0x05);  // itu_t_t35_terminal_provider_oriented_code
    payload.push_back(0x01);  // system_start_code

    detail::bit_writer_t writer { payload };
    writer.write(metadata.minimum_maxrgb_pq, 12);
    writer.write(metadata.average_maxrgb_pq, 12);
    writer.write(metadata.variance_maxrgb_pq, 12);
    writer.write(metadata.maximum_maxrgb_pq, 12);
    writer.write(0, 1);  // tone_mapping_mode_flag
    writer.write(0, 1);  // color_saturation_mapping_flag
    writer.flush();

    return payload.size();
  }

}  // namespace video::hdr_metadata
