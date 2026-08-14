/**
 * @file src/haptics/authored_ir.cpp
 * @brief Host-side authored DualSense PCM analysis and IR v2 serialization.
 */
#include "authored_ir.h"

#include <bit>
#include <cstring>

#include <moonlight_haptics/authored_haptics.h>

namespace haptics {
  namespace {
    constexpr std::uint8_t source_stream_start = 0x01;
    constexpr std::uint8_t source_stream_end = 0x02;
    constexpr std::uint8_t source_discontinuity = 0x04;
    constexpr std::uint8_t ir_stream_end = 0x04;
    constexpr std::uint8_t ir_silent = 0x08;

    void
    write_u16(std::uint8_t *p, std::uint16_t value) {
      p[0] = static_cast<std::uint8_t>(value);
      p[1] = static_cast<std::uint8_t>(value >> 8);
    }

    void
    write_u32(std::uint8_t *p, std::uint32_t value) {
      p[0] = static_cast<std::uint8_t>(value);
      p[1] = static_cast<std::uint8_t>(value >> 8);
      p[2] = static_cast<std::uint8_t>(value >> 16);
      p[3] = static_cast<std::uint8_t>(value >> 24);
    }

    void
    write_u64(std::uint8_t *p, std::uint64_t value) {
      write_u32(p, static_cast<std::uint32_t>(value));
      write_u32(p + 4, static_cast<std::uint32_t>(value >> 32));
    }

    void
    write_float(std::uint8_t *p, float value) {
      write_u32(p, std::bit_cast<std::uint32_t>(value));
    }
  }  // namespace

  void
  authored_ir_session_t::engine_deleter_t::operator()(AhAuthoredEngine *engine) const noexcept {
    ah_authored_destroy(engine);
  }

  authored_ir_session_t::authored_ir_session_t() {
    AhAuthoredConfig config {};
    AhAuthoredEngine *engine = nullptr;
    if (ah_authored_config_init(&config, 48000) == AH_STATUS_OK &&
        ah_authored_create(&config, &engine) == AH_STATUS_OK) {
      _engine.reset(engine);
    }
  }

  authored_ir_session_t::~authored_ir_session_t() = default;

  bool
  authored_ir_session_t::ready() const noexcept {
    return _engine != nullptr;
  }

  std::optional<authored_ir_v2_wire_t>
  authored_ir_session_t::process(std::uint16_t controller_id, std::uint8_t source_flags,
                                 std::uint16_t frame_count, std::uint32_t sequence,
                                 std::uint64_t presentation_time_us,
                                 std::span<const std::uint8_t> pcm) {
    const auto expected_pcm_size = static_cast<std::size_t>(frame_count) * 4;
    if (!_engine || frame_count > 240 || pcm.size() != expected_pcm_size) {
      return std::nullopt;
    }

    std::array<std::int16_t, 240 * 2> aligned_pcm {};
    if (!pcm.empty()) {
      std::memcpy(aligned_pcm.data(), pcm.data(), pcm.size());
    }
    AhAuthoredProcessInput input {};
    input.struct_size = AH_AUTHORED_PROCESS_INPUT_V2_SIZE;
    input.interleaved_pcm = aligned_pcm.data();
    input.frame_count = frame_count;
    input.first_sample_time_us = presentation_time_us;
    input.sequence_number = sequence;
    if (source_flags & source_stream_start) input.flags |= AH_AUTHORED_INPUT_STREAM_START;
    if (source_flags & source_stream_end) input.flags |= AH_AUTHORED_INPUT_STREAM_END;
    if (source_flags & source_discontinuity) input.flags |= AH_AUTHORED_INPUT_DISCONTINUITY;

    std::array<AhAuthoredHapticFrame, 1> output {};
    std::uint32_t output_count = 0;
    const auto status = ah_authored_process_i16(
      _engine.get(), &input, output.data(),
      static_cast<std::uint32_t>(output.size()), &output_count);
    if (status != AH_STATUS_OK && status != AH_STATUS_OUTPUT_AVAILABLE) {
      return std::nullopt;
    }

    authored_ir_v2_wire_t wire {};
    wire[0] = 2;
    write_u16(wire.data() + 2, authored_ir_v2_wire_size);
    write_u16(wire.data() + 4, controller_id);
    write_u32(wire.data() + 8, sequence);
    write_u64(wire.data() + 12, presentation_time_us);
    write_u32(wire.data() + 20, frame_count);
    if (output_count == 0 && (source_flags & source_stream_end)) {
      // An empty stream has no analyzable frame, but the transport must still
      // stop the client's actuator and clear its renderer state.
      wire[1] = ir_stream_end | ir_silent;
      return wire;
    }
    if (output_count != 1) {
      return std::nullopt;
    }

    const auto &frame = output[0];
    if (frame.flags & AH_AUTHORED_FRAME_DISCONTINUITY) wire[1] |= 0x01;
    if (frame.flags & AH_AUTHORED_FRAME_PARTIAL) wire[1] |= 0x02;
    if (frame.flags & AH_AUTHORED_FRAME_STREAM_END) wire[1] |= 0x04;
    if (frame.flags & AH_AUTHORED_FRAME_SILENT) wire[1] |= 0x08;
    write_u32(wire.data() + 8, frame.source_sequence_number);
    write_u64(wire.data() + 12, frame.timestamp_us);
    write_u32(wire.data() + 20, frame.source_frame_count);
    for (std::size_t lane = 0; lane < 2; ++lane) {
      auto *lane_wire = wire.data() + 24 + lane * 20;
      const auto &lane_frame = frame.lanes[lane];
      write_float(lane_wire, lane_frame.rms_amplitude);
      write_float(lane_wire + 4, lane_frame.peak_amplitude);
      write_float(lane_wire + 8, lane_frame.transient_strength);
      write_float(lane_wire + 12, lane_frame.low_band_ratio);
      write_float(lane_wire + 16, lane_frame.zero_crossing_rate_hz);
    }
    write_float(wire.data() + 64, frame.lane_correlation);
    write_u32(wire.data() + 68, 0);
    return wire;
  }
}  // namespace haptics
