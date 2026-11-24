/**
 * @file src/pyrowave/config.cpp
 * @brief Validation for the experimental PyroWave configuration.
 */
#include "config.h"

namespace pyrowave {

  namespace {

    [[nodiscard]] bool
    valid_api_version(const api_version_t &version) noexcept {
      return version.major != 0 || version.minor != 0 || version.patch != 0;
    }

  }  // namespace

  bool
  validate(const config_t &config) noexcept {
    if (!valid_api_version(config.expected_api_version) ||
        (config.chroma != chroma_e::yuv420 && config.chroma != chroma_e::yuv444) ||
        (config.transfer != transfer_e::bt709 && config.transfer != transfer_e::pq &&
         config.transfer != transfer_e::hlg) ||
        config.max_width == 0 || config.max_width > 16384 ||
        config.max_height == 0 || config.max_height > 16384 ||
        config.max_framerate == 0 || config.max_framerate > 240 ||
        config.max_bitrate_kbps == 0 || config.max_bitrate_kbps > 1000000) {
      return false;
    }

    if (config.packet_size != 0 && (config.packet_size < 256 || config.packet_size > 65535)) {
      return false;
    }

    if (config.chroma == chroma_e::yuv420 && ((config.max_width & 1u) != 0 || (config.max_height & 1u) != 0)) {
      return false;
    }

    return true;
  }

}  // namespace pyrowave
