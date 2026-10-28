// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

struct AhAuthoredEventEngine;

namespace haptics {
  // Sunshine adapter only: source S16LE and flags -> SDK -> IR v3 wire.
  // Device-independent DSP/state live in moonlight-audio-haptics authored_events.h.
  class authored_ir_v3_session_t {
  public:
    authored_ir_v3_session_t();
    ~authored_ir_v3_session_t();
    authored_ir_v3_session_t(const authored_ir_v3_session_t &) = delete;
    authored_ir_v3_session_t &
    operator=(const authored_ir_v3_session_t &) = delete;
    bool
    ready() const noexcept;
    std::optional<std::vector<std::uint8_t>>
    process(
      std::uint16_t controller_id, std::uint8_t source_flags,
      std::uint16_t frame_count, std::uint32_t source_sequence,
      std::uint64_t first_sample_time_us, std::span<const std::uint8_t> pcm);

  private:
    struct engine_deleter_t {
      void
      operator()(AhAuthoredEventEngine *engine) const noexcept;
    };
    std::unique_ptr<AhAuthoredEventEngine, engine_deleter_t> _engine;
  };
}  // namespace haptics
