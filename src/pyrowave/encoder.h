/**
 * @file src/pyrowave/encoder.h
 * @brief Encoder backend boundary for the experimental PyroWave path.
 */
#pragma once

#include "gpu_interop.h"
#include "packet.h"

namespace pyrowave {

  class encoder_t {
  public:
    virtual ~encoder_t() = default;

    [[nodiscard]] virtual failure_e
    prepare(const stream_config_t &config) noexcept = 0;

    [[nodiscard]] virtual failure_e
    encode(const gpu_frame_t &frame, frame_id_t frame_id, encoded_frame_t &output) noexcept = 0;

    virtual void
    stop() noexcept = 0;
  };

}  // namespace pyrowave
