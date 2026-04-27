/**
 * @file src/video_bitrate.h
 * @brief Shared video bitrate accounting helpers.
 */
#pragma once

namespace video {

  inline int
  dynamic_encoder_bitrate_kbps(int encoder_bitrate_kbps, int fec_percentage) {
    (void) fec_percentage;
    return encoder_bitrate_kbps;
  }
}  // namespace video
