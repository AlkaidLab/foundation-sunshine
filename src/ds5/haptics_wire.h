/**
 * @file src/ds5/haptics_wire.h
 * @brief Body of the DualSense haptics PCM control packet (the part after the control header).
 *
 * 28 bytes of header, little-endian, then 48 kHz signed 16-bit interleaved samples:
 *   byte 0       version 1
 *   byte 1       flags (1 stream start, 2 stream end, 4 discontinuity)
 *   bytes 2..3   header size (28)
 *   bytes 4..5   controller number
 *   bytes 6..7   frame count
 *   bytes 8..11  sequence
 *   bytes 12..19 presentation time in microseconds
 *   bytes 20..23 sample rate (48000)
 *   byte 24      channel count: 2 (haptic left, right) or 4 (speaker left, right, haptic left, right)
 *   byte 25      bits per sample (16)
 *   bytes 26..27 reserved, zero
 * Four channels are sent only to a client that offered "pad-audio=1"; every other client gets two.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ds5::haptics_wire {
  constexpr std::size_t header_bytes = 28;
  constexpr std::size_t max_frames = 240;

  /**
   * @param haptics frame_count * 4 bytes: haptic left and right per frame
   * @param speaker frame_count * 4 bytes: speaker left and right per frame, or nullptr for a two-channel packet
   */
  inline std::vector<std::uint8_t>
  build_pcm_body(std::uint8_t flags, std::uint16_t controller, std::uint16_t frame_count, std::uint32_t sequence,
    std::uint64_t presentation_time_us, const std::uint8_t *haptics, const std::uint8_t *speaker) {
    const std::size_t channels = speaker != nullptr ? 4 : 2;
    std::vector<std::uint8_t> body(header_bytes + static_cast<std::size_t>(frame_count) * channels * 2, 0);
    const auto put16 = [&](std::size_t at, std::uint16_t v) {
      body[at] = static_cast<std::uint8_t>(v);
      body[at + 1] = static_cast<std::uint8_t>(v >> 8);
    };
    const auto put32 = [&](std::size_t at, std::uint32_t v) {
      for (int i = 0; i < 4; ++i) body[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
    };
    body[0] = 1;
    body[1] = flags;
    put16(2, header_bytes);
    put16(4, controller);
    put16(6, frame_count);
    put32(8, sequence);
    put32(12, static_cast<std::uint32_t>(presentation_time_us));
    put32(16, static_cast<std::uint32_t>(presentation_time_us >> 32));
    put32(20, 48000);
    body[24] = static_cast<std::uint8_t>(channels);
    body[25] = 16;
    auto *out = body.data() + header_bytes;
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
      if (speaker != nullptr) {
        for (int i = 0; i < 4; ++i) *out++ = speaker[frame * 4 + i];
      }
      for (int i = 0; i < 4; ++i) *out++ = haptics[frame * 4 + i];
    }
    return body;
  }
}  // namespace ds5::haptics_wire
