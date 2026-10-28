// SPDX-License-Identifier: Apache-2.0
#include "authored_ir_v3.h"

#include <array>
#include <bit>
#include <moonlight_haptics/authored_events.h>

namespace haptics {
  namespace {
    void
    u16(std::uint8_t *p, std::uint16_t v) {
      p[0] = static_cast<std::uint8_t>(v);
      p[1] = static_cast<std::uint8_t>(v >> 8);
    }
    void
    u32(std::uint8_t *p, std::uint32_t v) {
      for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
    }
    void
    u64(std::uint8_t *p, std::uint64_t v) {
      for (unsigned i = 0; i < 8; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
    }
    void
    f32(std::uint8_t *p, float v) { u32(p, std::bit_cast<std::uint32_t>(v)); }
  }  // namespace

  void
  authored_ir_v3_session_t::engine_deleter_t::operator()(AhAuthoredEventEngine *engine) const noexcept {
    ah_authored_events_destroy(engine);
  }
  authored_ir_v3_session_t::authored_ir_v3_session_t() {
    AhAuthoredConfig config {};
    AhAuthoredEventEngine *engine = nullptr;
    if (ah_authored_config_init(&config, 48000) == AH_STATUS_OK &&
        ah_authored_events_create(&config, &engine) == AH_STATUS_OK) _engine.reset(engine);
  }
  authored_ir_v3_session_t::~authored_ir_v3_session_t() = default;
  bool
  authored_ir_v3_session_t::ready() const noexcept { return _engine != nullptr; }

  std::optional<std::vector<std::uint8_t>>
  authored_ir_v3_session_t::process(
    std::uint16_t controller, std::uint8_t source_flags, std::uint16_t count,
    std::uint32_t sequence, std::uint64_t timestamp, std::span<const std::uint8_t> pcm) {
    if (!_engine || count > 240 || pcm.size() != std::size_t(count) * 4 || (source_flags & ~7u))
      return std::nullopt;
    std::array<std::int16_t, 480> aligned {};
    for (std::size_t i = 0; i < std::size_t(count) * 2; ++i) {
      aligned[i] = std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(
        std::uint16_t(pcm[i * 2]) | (std::uint16_t(pcm[i * 2 + 1]) << 8)));
    }
    AhAuthoredProcessInput input {};
    input.struct_size = AH_AUTHORED_PROCESS_INPUT_V2_SIZE;
    input.interleaved_pcm = aligned.data();
    input.frame_count = count;
    input.sequence_number = sequence;
    input.first_sample_time_us = timestamp;
    if (source_flags & 1) input.flags |= AH_AUTHORED_INPUT_STREAM_START;
    if (source_flags & 2) input.flags |= AH_AUTHORED_INPUT_STREAM_END;
    if (source_flags & 4) input.flags |= AH_AUTHORED_INPUT_DISCONTINUITY;
    AhAuthoredEventFrame frame {};
    std::uint32_t output_count = 0;
    if (ah_authored_events_process_i16(_engine.get(), &input, &frame, 1, &output_count) !=
          AH_STATUS_OUTPUT_AVAILABLE ||
        output_count != 1) return std::nullopt;

    std::vector<std::uint8_t> wire(64 + frame.event_count * 32, 0);
    wire[0] = 3;
    wire[1] = static_cast<std::uint8_t>(frame.flags);
    u16(wire.data() + 2, static_cast<std::uint16_t>(wire.size()));
    u16(wire.data() + 4, controller);
    u16(wire.data() + 6, static_cast<std::uint16_t>(frame.profile));
    u32(wire.data() + 8, frame.epoch);
    u32(wire.data() + 12, frame.frame_sequence);
    u64(wire.data() + 16, frame.timestamp_us);
    u32(wire.data() + 24, frame.source_frame_count);
    wire[28] = static_cast<std::uint8_t>(frame.event_count);
    wire[29] = 2;
    for (unsigned i = 0; i < 2; ++i) {
      auto *lane = wire.data() + 32 + i * 16;
      f32(lane, frame.lanes[i].continuous_low);
      f32(lane + 4, frame.lanes[i].continuous_high);
      f32(lane + 8, frame.lanes[i].raw_rms);
      f32(lane + 12, frame.lanes[i].raw_peak);
    }
    for (unsigned i = 0; i < frame.event_count; ++i) {
      const auto &event = frame.events[i];
      auto *out = wire.data() + 64 + i * 32;
      u32(out, event.event_id);
      u64(out + 8, event.source_time_us);
      f32(out + 16, event.left_strength);
      f32(out + 20, event.right_strength);
      f32(out + 24, event.confidence);
      f32(out + 28, event.sharpness);
    }
    return wire;
  }
}  // namespace haptics
