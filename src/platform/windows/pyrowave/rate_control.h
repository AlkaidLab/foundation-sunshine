#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "src/streaming/fec.h"

extern "C" {
#include "third-party/moonlight-common-c/src/PyrowaveProtocol.h"
}

namespace platf::pyrowave_windows {
  inline constexpr std::size_t pyrowave_max_bitstream_size = 16u * 1024u * 1024u;
  // Keep one stable frame budget below the legacy RTP four-block burst size.
  // The storage buffer remains 16 MiB, but allowing a larger per-frame target
  // creates a burst that can be visible as flicker on the client.
  inline constexpr std::size_t pyrowave_max_frame_budget = 1u * 1024u * 1024u;
  // The PyroWave API only needs a small positive target containing its
  // sequence header. The actual output buffer is independently reserved at
  // 16 MiB below; do not use that buffer capacity as a bitrate floor.
  inline constexpr std::size_t pyrowave_min_bitstream_size = 8u;
  inline constexpr std::size_t pyrowave_sequence_header_size = 8u;
  // The optional host processing latency TLV has an 8-byte header and 2-byte value.
  inline constexpr std::size_t pyrowave_runtime_metadata_reserve = 10u;

  // A PYRF frame has one separate frame header, N data shards and ceil(N/16)
  // parity shards. Each takes one outer RTP payload. Reserve metadata before
  // giving the remaining bytes to the codec; HDR metadata is generated only
  // after encoding, so its maximum supported size must be reserved in advance.
  [[nodiscard]] inline constexpr std::size_t
  pyrowave_transport_bitstream_capacity(std::size_t payload_boundary, std::size_t metadata_reserve) noexcept {
    constexpr auto available_shards = video_fec::max_data_shards_per_frame - 1u;
    constexpr auto group_size = static_cast<std::size_t>(LI_PYROWAVE_FEC_DATA_PER_GROUP);
    constexpr auto max_data_shards = available_shards - (available_shards + group_size) / (group_size + 1u);
    if (payload_boundary == 0 || payload_boundary > LI_PYROWAVE_MAX_PACKET_SIZE - LI_PYROWAVE_WIRE_FEC_HEADER_SIZE) {
      return 0;
    }
    const auto protected_capacity = max_data_shards * payload_boundary;
    return protected_capacity > metadata_reserve ? protected_capacity - metadata_reserve : 0;
  }

  // Use the negotiated frame rate. Capture wait timeouts and static-content
  // refresh floors do not cap how quickly new images can be encoded.
  [[nodiscard]] inline std::size_t
  pyrowave_frame_budget(int bitrate_kbps, int frame_rate_num, int frame_rate_den) noexcept {
    if (bitrate_kbps <= 0 || frame_rate_num <= 0 || frame_rate_den <= 0) {
      return pyrowave_max_frame_budget;
    }
    const auto bits = static_cast<std::uint64_t>(bitrate_kbps) * 1000u *
      static_cast<std::uint64_t>(frame_rate_den);
    const auto bytes = bits / 8u / static_cast<std::uint64_t>(frame_rate_num);
    return std::clamp<std::size_t>(static_cast<std::size_t>(bytes),
      pyrowave_min_bitstream_size, pyrowave_max_frame_budget);
  }

  [[nodiscard]] inline std::size_t
  pyrowave_bounded_frame_budget(int bitrate_kbps, int frame_rate_num, int frame_rate_den,
                               std::size_t payload_boundary, std::size_t metadata_reserve) noexcept {
    const auto capacity = pyrowave_transport_bitstream_capacity(payload_boundary, metadata_reserve);
    if (capacity < pyrowave_sequence_header_size + pyrowave_min_bitstream_size) return 0;
    // Keep explicit headroom for the sequence header even though the current
    // PyroWave RDO also accounts for it. The actual output is checked later.
    return std::min(pyrowave_frame_budget(bitrate_kbps, frame_rate_num, frame_rate_den),
                    capacity - pyrowave_sequence_header_size);
  }

  [[nodiscard]] inline double
  pyrowave_frame_bits_per_pixel(
    std::size_t frame_budget, int width, int height) noexcept {
    if (frame_budget == 0 || width <= 0 || height <= 0) {
      return 0.0;
    }
    return static_cast<double>(frame_budget) * 8.0 /
      (static_cast<double>(width) * static_cast<double>(height));
  }
}  // namespace platf::pyrowave_windows
