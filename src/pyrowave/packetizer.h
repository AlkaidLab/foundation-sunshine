/**
 * @file src/pyrowave/packetizer.h
 * @brief Packetization boundary between PyroWave frames and Sunshine video packets.
 */
#pragma once

#include "packet.h"

#include <memory>

namespace pyrowave {

  class packetizer_t {
  public:
    virtual ~packetizer_t() = default;

    [[nodiscard]] virtual packetization_result_t
    packetize(
      const encoded_frame_t &frame,
      const packetization_request_t &request) noexcept = 0;

    virtual void
    reset() noexcept = 0;
  };

  /**
   * Create the stateless server transport packetizer used by the experimental
   * path. It wraps an already encoded frame and never selects a legacy codec.
   */
  [[nodiscard]] std::unique_ptr<packetizer_t>
  make_transport_packetizer();

}  // namespace pyrowave
