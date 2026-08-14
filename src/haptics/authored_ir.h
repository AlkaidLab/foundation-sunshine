/**
 * @file src/haptics/authored_ir.h
 * @brief Host-side authored DualSense PCM analysis and IR v2 serialization.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

struct AhAuthoredEngine;

namespace haptics {
  constexpr std::size_t authored_ir_v2_wire_size = 72;
  using authored_ir_v2_wire_t = std::array<std::uint8_t, authored_ir_v2_wire_size>;

  class authored_ir_session_t {
  public:
    authored_ir_session_t();
    ~authored_ir_session_t();

    authored_ir_session_t(const authored_ir_session_t &) = delete;
    authored_ir_session_t &operator=(const authored_ir_session_t &) = delete;

    bool
    ready() const noexcept;

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
}  // namespace haptics
