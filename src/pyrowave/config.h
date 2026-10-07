/**
 * @file src/pyrowave/config.h
 * @brief Configuration boundary for the optional PyroWave feature.
 */
#pragma once

#include "types.h"

#include <cstdint>

namespace pyrowave {

  struct config_t {
    bool enabled = false;
    bool allow_experimental_client = false;
    bool require_external_interop = false;
    bool require_format_conversion = false;
    bool require_timeline_sync = false;
    api_version_t expected_api_version { 0, 6, 1 };
    std::uint32_t max_width = 1920;
    std::uint32_t max_height = 1080;
    std::uint32_t max_framerate = 60;
    std::uint32_t max_bitrate_kbps = 250000;
    std::uint32_t packet_size = 0;
    chroma_e chroma = chroma_e::yuv420;
    transfer_e transfer = transfer_e::bt709;
  };

  [[nodiscard]] bool
  validate(const config_t &config) noexcept;

}  // namespace pyrowave
