/**
 * @file src/streaming/video_pacing.h
 * @brief Rate selection and packet time slots for large video frames.
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace streaming {

  constexpr std::uint64_t video_pacing_headroom_percent = 80;

  inline std::uint64_t
  video_pacing_rate_bps(int measured_kbps, int configured_kbps) noexcept {
    // A measurement above the encoder budget is a lower bound on the path, so
    // it is used as the path rate. A lower measurement is this process's own
    // recent output (the meter divides by the whole window) and pacing from it
    // ratchets down. fs-rfc8085-udp-20261112 section 3.1.4: otherwise pace at
    // the configured rate with headroom, not at line rate.
    if (measured_kbps > configured_kbps && measured_kbps > 0) {
      return static_cast<std::uint64_t>(measured_kbps) * 1000;
    }
    if (configured_kbps <= 0) return 0;
    return static_cast<std::uint64_t>(configured_kbps) * 1000 * video_pacing_headroom_percent / 100;
  }

  inline bool
  should_pace_video_frame(bool key_frame, std::size_t frame_bytes, std::size_t burst_budget_bytes) noexcept {
    (void) key_frame;
    return burst_budget_bytes != 0 && frame_bytes >= burst_budget_bytes;
  }

  // fs-rfc8085-udp-20261112 section 3.1.4 recommends regulating UDP bursts. A slot is the
  // elapsed time at which packet_index may be submitted at rate_bps.
  inline std::chrono::nanoseconds
  video_packet_slot(std::size_t packet_index, std::size_t packet_bytes, std::uint64_t rate_bps) noexcept {
    if (rate_bps == 0 || packet_bytes == 0 || packet_index == 0) return {};
    const auto bits = static_cast<long double>(packet_index) * static_cast<long double>(packet_bytes) * 8.0L;
    const auto seconds = bits / static_cast<long double>(rate_bps);
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<long double> { seconds });
  }

}  // namespace streaming
