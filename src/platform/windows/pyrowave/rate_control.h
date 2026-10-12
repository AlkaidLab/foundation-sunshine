#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

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
