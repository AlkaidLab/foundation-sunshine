// SPDX-License-Identifier: Apache-2.0
#include "authored_ir_extended.h"
#include "authored_input.h"
#include "wire.h"

#include <algorithm>
#include <moonlight_haptics/authored_events.h>

namespace haptics {
  namespace {
    using wire::write_float;
    using wire::write_u16;
    using wire::write_u32;
    using wire::write_u64;
  }  // namespace

  void
  authored_ir_extended_session_t::engine_deleter_t::operator()(AhAuthoredEventEngine *engine) const noexcept {
    ah_authored_events_destroy(engine);
  }
  authored_ir_extended_session_t::authored_ir_extended_session_t() {
    AhAuthoredConfig config {};
    AhAuthoredEventEngine *engine = nullptr;
    if (ah_authored_config_init(&config, 48000) == AH_STATUS_OK &&
        ah_authored_events_create(&config, &engine) == AH_STATUS_OK) _engine.reset(engine);
  }
  authored_ir_extended_session_t::~authored_ir_extended_session_t() = default;
  bool
  authored_ir_extended_session_t::ready() const noexcept { return _engine != nullptr && _base.ready(); }

  std::optional<std::vector<std::uint8_t>>
  authored_ir_extended_session_t::process(
    std::uint16_t controller, std::uint8_t source_flags, std::uint16_t count,
    std::uint32_t sequence, std::uint64_t timestamp, std::span<const std::uint8_t> pcm) {
    if (!_engine || (source_flags & ~7u)) return std::nullopt;
    detail::authored_pcm_t aligned {};
    const auto input = detail::prepare_authored_input(
      source_flags, count, sequence, timestamp, pcm, aligned);
    if (!input) return std::nullopt;
    AhAuthoredEventFrame frame {};
    std::uint32_t output_count = 0;
    if (ah_authored_events_process_i16(_engine.get(), &*input, &frame, 1, &output_count) !=
          AH_STATUS_OUTPUT_AVAILABLE ||
        output_count != 1 || frame.event_count > LI_DS5_HAPTICS_IR_EXTENDED_MAX_EVENTS) return std::nullopt;

    // The legacy analyzer and writer remain the sole source of the v2 base.
    // Small input chunks may not complete its 5 ms hop yet.
    const auto base = _base.process(controller, source_flags, count, sequence, timestamp, pcm);
    if (!base) return std::nullopt;
    std::vector<std::uint8_t> wire(DS5_HAPTICS_IR_EXTENDED_STREAM_BASE_SIZE +
                                  frame.event_count * DS5_HAPTICS_IR_EXTENDED_STREAM_EVENT_SIZE, 0);
    std::ranges::copy(*base, wire.begin());
    auto *extension = wire.data() + authored_ir_v2_wire_size;
    extension[0] = DS5_HAPTICS_IR_EXTENSION_VERSION;
    extension[1] = static_cast<std::uint8_t>(frame.event_count);
    write_u16(extension + 2, static_cast<std::uint16_t>(wire.size() - authored_ir_v2_wire_size));
    write_u32(extension + 4, frame.epoch);
    write_u64(extension + 8, frame.timestamp_us);
    for (unsigned i = 0; i < 2; ++i) {
      write_float(extension + 16 + i * 8, frame.lanes[i].continuous_low);
      write_float(extension + 20 + i * 8, frame.lanes[i].continuous_high);
    }
    for (unsigned i = 0; i < frame.event_count; ++i) {
      const auto &event = frame.events[i];
      auto *out = wire.data() + DS5_HAPTICS_IR_EXTENDED_STREAM_BASE_SIZE +
                  i * DS5_HAPTICS_IR_EXTENDED_STREAM_EVENT_SIZE;
      write_u32(out, event.event_id);
      write_u64(out + 4, event.source_time_us);
      write_float(out + 12, event.left_strength);
      write_float(out + 16, event.right_strength);
    }
    return wire;
  }
}  // namespace haptics
