// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <moonlight_haptics/authored_haptics.h>

namespace haptics::detail {
  using authored_pcm_t = std::array<std::int16_t, 240 * 2>;

  // The caller owns aligned_pcm for the duration of the SDK call.
  inline std::optional<AhAuthoredProcessInput>
  prepare_authored_input(std::uint8_t source_flags, std::uint16_t frame_count,
    std::uint32_t sequence, std::uint64_t timestamp,
    std::span<const std::uint8_t> pcm, authored_pcm_t &aligned_pcm) {
    if (frame_count > aligned_pcm.size() / 2 || pcm.size() != std::size_t(frame_count) * 4)
      return std::nullopt;
    if constexpr (std::endian::native == std::endian::little) {
      if (!pcm.empty()) std::memcpy(aligned_pcm.data(), pcm.data(), pcm.size());
    }
    else {
      for (std::size_t i = 0; i < std::size_t(frame_count) * 2; ++i) {
        aligned_pcm[i] = std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(
          std::uint16_t(pcm[i * 2]) | (std::uint16_t(pcm[i * 2 + 1]) << 8)));
      }
    }
    AhAuthoredProcessInput input {};
    input.struct_size = AH_AUTHORED_PROCESS_INPUT_V2_SIZE;
    input.interleaved_pcm = aligned_pcm.data();
    input.frame_count = frame_count;
    input.sequence_number = sequence;
    input.first_sample_time_us = timestamp;
    if (source_flags & 1) input.flags |= AH_AUTHORED_INPUT_STREAM_START;
    if (source_flags & 2) input.flags |= AH_AUTHORED_INPUT_STREAM_END;
    if (source_flags & 4) input.flags |= AH_AUTHORED_INPUT_DISCONTINUITY;
    return input;
  }
}  // namespace haptics::detail
