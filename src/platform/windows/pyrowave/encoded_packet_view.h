#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace platf::pyrowave_windows {
  // Returned bytes borrow either input or scratch, both of which must remain
  // alive until synchronous transport packetization has finished.
  template<class Packet>
  [[nodiscard]] bool
  collect_encoded_packets(std::span<const std::uint8_t> input,
                          std::span<const Packet> packets,
                          std::vector<std::uint8_t> &scratch,
                          std::span<const std::uint8_t> &output) {
    output = {};
    std::size_t total = 0;
    bool contiguous = true;
    for (const auto &packet : packets) {
      if (packet.offset > input.size() || packet.size > input.size() - packet.offset ||
          packet.size > input.size() - total) {
        return false;
      }
      contiguous &= packet.offset == total;
      total += packet.size;
    }
    if (contiguous) {
      output = input.first(total);
      return true;
    }
    scratch.clear();
    scratch.reserve(total);
    for (const auto &packet : packets) {
      const auto bytes = input.subspan(packet.offset, packet.size);
      scratch.insert(scratch.end(), bytes.begin(), bytes.end());
    }
    output = scratch;
    return true;
  }
}  // namespace platf::pyrowave_windows
