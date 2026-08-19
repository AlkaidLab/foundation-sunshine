/**
 * @file src/haptics/authored_ir.h
 * @brief Host-side authored DualSense PCM analysis and IR v2 serialization.
 */
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

struct AhAuthoredEngine;

namespace haptics {
  constexpr std::size_t authored_ir_v2_wire_size = 72;
  using authored_ir_v2_wire_t = std::array<std::uint8_t, authored_ir_v2_wire_size>;

  struct authored_lane_t {
    float rms_amplitude;
    float peak_amplitude;
    float transient_strength;
    float low_band_ratio;
    float zero_crossing_rate_hz;
  };

  struct authored_frame_t {
    std::uint32_t flags;
    std::uint64_t timestamp_us;
    std::uint32_t source_sequence_number;
    std::uint32_t source_frame_count;
    std::array<authored_lane_t, 2> lanes;
    float lane_correlation;
  };

  class authored_ir_session_t {
  public:
    authored_ir_session_t();
    ~authored_ir_session_t();

    authored_ir_session_t(const authored_ir_session_t &) = delete;
    authored_ir_session_t &operator=(const authored_ir_session_t &) = delete;

    bool
    ready() const noexcept;

    std::optional<authored_frame_t>
    analyze(std::uint8_t source_flags, std::uint16_t frame_count,
            std::uint32_t sequence, std::uint64_t presentation_time_us,
            std::span<const std::uint8_t> pcm);

    std::optional<authored_ir_v2_wire_t>
    process(std::uint16_t controller_id, std::uint8_t source_flags,
            std::uint16_t frame_count, std::uint32_t sequence,
            std::uint64_t presentation_time_us,
            std::span<const std::uint8_t> pcm);

  private:
    struct engine_deleter_t {
      void operator()(AhAuthoredEngine *engine) const noexcept;
    };
    std::unique_ptr<AhAuthoredEngine, engine_deleter_t> _engine;
  };

  struct legacy_rumble_t {
    std::uint16_t controller_id;
    std::uint16_t low_frequency;
    std::uint16_t high_frequency;
  };

  /**
   * Noise-gate state for one motor. The hold budget bounds how long hysteresis
   * may keep the gate open without the input re-crossing the open threshold.
   */
  struct gate_state_t {
    bool open = false;
    float hold_seconds = 0.0f;
  };

  /**
   * Converts authored DualSense actuator PCM into the two-motor rumble format
   * understood by legacy Moonlight clients.
   */
  class legacy_rumble_session_t {
  public:
    bool
    ready() const noexcept;

    std::optional<legacy_rumble_t>
    process(std::uint16_t controller_id, std::uint8_t source_flags,
            std::uint16_t frame_count, std::uint32_t sequence,
            std::uint64_t presentation_time_us, std::span<const std::uint8_t> pcm,
            std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

    std::optional<legacy_rumble_t>
    poll(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

  private:
    authored_ir_session_t _analyzer;
    std::chrono::steady_clock::time_point _last_input {};
    std::chrono::steady_clock::time_point _last_emit {};
    std::chrono::steady_clock::time_point _active_since {};
    std::uint16_t _controller_id = 0;
    std::uint16_t _last_low = 0;
    std::uint16_t _last_high = 0;
    std::uint16_t _last_nonzero_low = 0;
    std::uint16_t _last_nonzero_high = 0;
    float _smoothed_low = 0.0f;
    float _smoothed_high = 0.0f;
    gate_state_t _low_gate;
    gate_state_t _high_gate;
    // Set only when a motor starts releasing before the short-pulse hold
    // expires. Such pulses are force-cleared at the boundary; long effects
    // retain their normal release tail.
    bool _short_release_low = false;
    bool _short_release_high = false;
    bool _have_input = false;
  };
}  // namespace haptics
