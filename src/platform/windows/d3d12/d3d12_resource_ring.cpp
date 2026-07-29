/**
 * @file src/platform/windows/d3d12/d3d12_resource_ring.cpp
 * @brief Non-blocking slot lifecycle for the Windows D3D12 video pipeline.
 */
#include "d3d12_resource_ring.h"

#include <algorithm>
#include <stdexcept>

namespace platf::dxgi::d3d12 {
  bool
  resource_ring_t::begin_generation(std::uint64_t generation) {
    if (generation == 0 || generation <= generation_ ||
        std::ranges::any_of(slots_, [](const auto &slot) {
          return slot.state != slot_state_e::free;
        })) {
      return false;
    }

    generation_ = generation;
    next_slot_ = 0;
    return true;
  }

  std::optional<std::size_t>
  resource_ring_t::try_acquire(std::uint64_t completed_fence) {
    if (generation_ == 0) {
      return std::nullopt;
    }
    retire_completed(completed_fence);
    for (std::size_t offset = 0; offset < slot_count; ++offset) {
      const auto index = (next_slot_ + offset) % slot_count;
      auto &candidate = slots_[index];
      if (candidate.state != slot_state_e::free) {
        continue;
      }

      candidate = {};
      candidate.state = slot_state_e::capture_writing;
      candidate.generation = generation_;
      next_slot_ = (index + 1) % slot_count;
      high_watermark_ = std::max(high_watermark_, busy_slots());
      return index;
    }
    return std::nullopt;
  }

  bool
  resource_ring_t::mark_capture_ready(
    std::size_t index,
    std::uint64_t fence_value) {
    if (index >= slot_count ||
        slots_[index].state != slot_state_e::capture_writing ||
        !record_fence(fence_value)) {
      return false;
    }
    slots_[index].capture_ready = fence_value;
    slots_[index].state = slot_state_e::capture_ready;
    return true;
  }

  bool
  resource_ring_t::cancel_capture(std::size_t index) {
    if (index >= slot_count ||
        slots_[index].state != slot_state_e::capture_writing) {
      return false;
    }
    slots_[index] = {};
    return true;
  }

  bool
  resource_ring_t::mark_compute_queued(
    std::size_t index,
    std::uint64_t fence_value,
    bool analysis_dispatched) {
    if (index >= slot_count ||
        slots_[index].state != slot_state_e::capture_ready ||
        !record_fence(fence_value)) {
      return false;
    }
    slots_[index].compute_done = fence_value;
    slots_[index].analysis_readback_released = !analysis_dispatched;
    slots_[index].state = slot_state_e::compute_queued;
    return true;
  }

  bool
  resource_ring_t::mark_encoder_queued(
    std::size_t index,
    std::uint64_t fence_value) {
    if (index >= slot_count ||
        slots_[index].state != slot_state_e::compute_queued ||
        !record_fence(fence_value)) {
      return false;
    }
    slots_[index].encode_done = fence_value;
    slots_[index].state = slot_state_e::encoder_queued;
    return true;
  }

  bool
  resource_ring_t::release_analysis_readback(
    std::size_t index,
    std::uint64_t generation) {
    if (index >= slot_count ||
        slots_[index].generation != generation ||
        (slots_[index].state != slot_state_e::compute_queued &&
          slots_[index].state != slot_state_e::encoder_queued)) {
      return false;
    }
    slots_[index].analysis_readback_released = true;
    return true;
  }

  void
  resource_ring_t::retire_completed(std::uint64_t completed_fence) {
    for (auto &slot : slots_) {
      if (slot.state == slot_state_e::encoder_queued &&
          slot.encode_done <= completed_fence &&
          slot.analysis_readback_released) {
        slot = {};
      }
    }
  }

  const resource_slot_t &
  resource_ring_t::slot(std::size_t index) const {
    if (index >= slot_count) {
      throw std::out_of_range("D3D12 resource ring slot index");
    }
    return slots_[index];
  }

  std::uint64_t
  resource_ring_t::generation() const {
    return generation_;
  }

  std::size_t
  resource_ring_t::high_watermark() const {
    return high_watermark_;
  }

  bool
  resource_ring_t::record_fence(std::uint64_t fence_value) {
    if (fence_value == 0 || fence_value <= last_fence_value_) {
      return false;
    }
    last_fence_value_ = fence_value;
    return true;
  }

  std::size_t
  resource_ring_t::busy_slots() const {
    return std::ranges::count_if(slots_, [](const auto &slot) {
      return slot.state != slot_state_e::free;
    });
  }
}  // namespace platf::dxgi::d3d12
