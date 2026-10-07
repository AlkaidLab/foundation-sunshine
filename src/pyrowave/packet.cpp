/**
 * @file src/pyrowave/packet.cpp
 * @brief Validation for the transport-neutral PyroWave packet contracts.
 */
#include "packet.h"

namespace pyrowave {

  bool
  validate(const encoded_frame_t &frame) noexcept {
    return frame.frame_id != 0 && frame.kind == frame_kind_e::intra && !frame.bitstream.empty() &&
           validate(frame.deadline);
  }

  bool
  validate(const packetization_request_t &request) noexcept {
    return request.packet_boundary == 0 || request.packet_boundary <= 65535;
  }

  bool
  validate(const packetization_result_t &result) noexcept {
    if (result.failure != failure_e::none) {
      return result.packets.empty() && result.bitstream.empty();
    }

    const auto bitstream = result.bitstream.view();
    if (result.packets.empty()) {
      return false;
    }

    if (bitstream.empty()) {
      return false;
    }

    for (const auto &packet: result.packets) {
      if (packet.size == 0 || packet.offset > bitstream.size() || packet.size > bitstream.size() - packet.offset) {
        return false;
      }
    }

    return true;
  }

  bool
  validate(const reassembly_policy_t &policy) noexcept {
    return policy.deadline.count() >= 0;
  }

}  // namespace pyrowave
