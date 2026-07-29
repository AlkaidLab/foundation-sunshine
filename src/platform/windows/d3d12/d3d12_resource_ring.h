/**
 * @file src/platform/windows/d3d12/d3d12_resource_ring.h
 * @brief Non-blocking slot lifecycle for the Windows D3D12 video pipeline.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace platf::dxgi::d3d12 {
  enum class slot_state_e {
    free,
    capture_writing,
    capture_ready,
    compute_queued,
    encoder_queued,
  };

  struct resource_slot_t {
    slot_state_e state = slot_state_e::free;
    std::uint64_t generation = 0;
    std::uint64_t capture_ready = 0;
    std::uint64_t compute_done = 0;
    std::uint64_t encode_done = 0;
    bool analysis_readback_released = true;
  };

  class resource_ring_t {
  public:
    static constexpr std::size_t slot_count = 3;

    [[nodiscard]] bool
    begin_generation(std::uint64_t generation);

    [[nodiscard]] std::optional<std::size_t>
    try_acquire(std::uint64_t completed_fence);

    [[nodiscard]] bool
    mark_capture_ready(std::size_t index, std::uint64_t fence_value);

    [[nodiscard]] bool
    cancel_capture(std::size_t index);

    [[nodiscard]] bool
    mark_compute_queued(
      std::size_t index,
      std::uint64_t fence_value,
      bool analysis_dispatched);

    [[nodiscard]] bool
    mark_encoder_queued(std::size_t index, std::uint64_t fence_value);

    [[nodiscard]] bool
    release_analysis_readback(std::size_t index, std::uint64_t generation);

    void
    retire_completed(std::uint64_t completed_fence);

    [[nodiscard]] const resource_slot_t &
    slot(std::size_t index) const;

    [[nodiscard]] std::uint64_t
    generation() const;

    [[nodiscard]] std::size_t
    high_watermark() const;

  private:
    [[nodiscard]] bool
    record_fence(std::uint64_t fence_value);

    [[nodiscard]] std::size_t
    busy_slots() const;

    std::array<resource_slot_t, slot_count> slots_ {};
    std::uint64_t generation_ = 0;
    std::uint64_t last_fence_value_ = 0;
    std::size_t next_slot_ = 0;
    std::size_t high_watermark_ = 0;
  };
}  // namespace platf::dxgi::d3d12
