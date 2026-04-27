/**
 * @file src/stream_bitrate.h
 * @brief Shared stream bitrate budget helpers.
 */
#pragma once

#include <algorithm>
#include <cstdint>
#include <cmath>

namespace stream_bitrate {

  inline std::int64_t
  encoding_bitrate_from_configured_total_kbps(std::int64_t configured_bitrate_kbps,
                                              int fec_percentage,
                                              bool high_quality_audio,
                                              int audio_channels) {
    (void) high_quality_audio;
    (void) audio_channels;

    if (configured_bitrate_kbps <= 0) {
      return configured_bitrate_kbps;
    }

    fec_percentage = std::clamp(fec_percentage, 0, 80);
    if (fec_percentage == 0) {
      return configured_bitrate_kbps;
    }

    return static_cast<std::int64_t>(std::lround(
      static_cast<double>(configured_bitrate_kbps) *
      static_cast<double>(100 - fec_percentage) / 100.0));
  }
}  // namespace stream_bitrate
