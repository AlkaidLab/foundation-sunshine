/**
 * @file src/pyrowave/types.cpp
 * @brief Validation for platform-neutral PyroWave value types.
 */
#include "types.h"

namespace pyrowave {

  bool
  validate(const stream_config_t &config) noexcept {
    if ((config.chroma != chroma_e::yuv420 && config.chroma != chroma_e::yuv444) ||
        (config.transfer != transfer_e::bt709 && config.transfer != transfer_e::pq &&
         config.transfer != transfer_e::hlg) || config.width == 0 ||
        config.width > 16384 || config.height == 0 || config.height > 16384 || config.framerate_num == 0 ||
        config.framerate_den == 0) {
      return false;
    }

    if (config.packet_size != 0 && (config.packet_size < 256 || config.packet_size > 65535)) {
      return false;
    }

    return config.chroma != chroma_e::yuv420 || ((config.width & 1u) == 0 && (config.height & 1u) == 0);
  }

  bool
  validate(const frame_deadline_t &deadline) noexcept {
    return deadline.budget.count() >= 0;
  }

}  // namespace pyrowave
