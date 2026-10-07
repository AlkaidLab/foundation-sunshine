/**
 * @file src/pyrowave/packet.cpp
 * @brief Validation for the transport-neutral PyroWave packet contracts.
 */
#include "packet.h"

#include <algorithm>

extern "C" {
#include "third-party/moonlight-common-c/src/PyrowaveProtocol.h"
}

namespace pyrowave {

  int
  limit_rtp_packet_size(int requested_packet_size, std::uint32_t max_wire_packet_size) noexcept {
    constexpr std::uint32_t outer_header_overhead = 16;
    if (requested_packet_size <= static_cast<int>(outer_header_overhead) ||
        max_wire_packet_size > LI_PYROWAVE_MAX_PACKET_SIZE) {
      return 0;
    }
    const auto wire_size = std::min(
      static_cast<std::uint32_t>(requested_packet_size) - outer_header_overhead, max_wire_packet_size);
    if (wire_size <= 2 * LI_PYROWAVE_WIRE_HEADER_SIZE) {
      return 0;
    }
    return static_cast<int>(wire_size + outer_header_overhead);
  }

  bool
  validate(const encoded_frame_t &frame) noexcept {
    return frame.frame_id != 0 && frame.kind == frame_kind_e::intra && !frame.bitstream.empty() &&
           validate(frame.deadline);
  }

  bool
  validate(const packetization_request_t &request) noexcept {
    const auto boundary = request.packet_boundary == 0 ? LI_PYROWAVE_MAX_PACKET_SIZE : request.packet_boundary;
    // FRAME_HEADER carries a complete metadata copy in one packet, even
    // though the protected payload may span multiple DATA packets.
    return boundary > LI_PYROWAVE_WIRE_HEADER_SIZE &&
           boundary <= LI_PYROWAVE_MAX_PACKET_SIZE &&
           request.metadata.size() <= boundary - LI_PYROWAVE_WIRE_HEADER_SIZE;
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
