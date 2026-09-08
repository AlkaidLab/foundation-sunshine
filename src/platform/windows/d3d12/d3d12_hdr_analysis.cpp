/**
 * @file src/platform/windows/d3d12/d3d12_hdr_analysis.cpp
 * @brief D3D12 HDR analyzer lifecycle, submission and asynchronous readback.
 */
#include "d3d12_hdr_analysis_internal.h"

#include <cstring>

namespace platf::dxgi::d3d12 {
  using namespace hdr_analysis_detail;

  hdr_analysis_t::hdr_analysis_t():
      impl_(std::make_unique<impl_t>()) {
  }

  hdr_analysis_t::~hdr_analysis_t() {
    disable();
  }

  hdr_analysis_init_result_t
  hdr_analysis_t::initialize(
    device_t &foundation,
    ID3D11Device *d3d11_device,
    ID3D11DeviceContext *d3d11_context,
    std::uint32_t analysis_width,
    std::uint32_t analysis_height,
    std::uint32_t source_width,
    std::uint32_t source_height,
    float max_analysis_nits,
    std::uint64_t generation) {
    disable();
    impl_ = std::make_unique<impl_t>();
    if (!foundation.available() || !d3d11_device || !d3d11_context ||
        analysis_width == 0 || analysis_height == 0 ||
        source_width == 0 || source_height == 0 || generation == 0) {
      impl_->fail(E_INVALIDARG, "hdr_input_validation");
      return { false, impl_->failure_hresult, impl_->failure_stage };
    }

    impl_->foundation = &foundation;
    impl_->analysis_width = analysis_width;
    impl_->analysis_height = analysis_height;
    impl_->source_width = source_width;
    impl_->source_height = source_height;
    impl_->max_analysis_nits = max_analysis_nits;
    impl_->num_groups =
      ((analysis_width + 15) / 16) * ((analysis_height + 15) / 16);
    auto status = d3d11_context->QueryInterface(
      IID_PPV_ARGS(&impl_->d3d11_context4));
    if (FAILED(status)) {
      impl_->fail(status, "hdr_d3d11_context4");
    }
    if (SUCCEEDED(status)) {
      status = impl_->create_shared_fence_view(d3d11_device);
    }
    if (SUCCEEDED(status)) {
      status = impl_->create_pipeline();
    }
    if (SUCCEEDED(status)) {
      D3D12_DESCRIPTOR_HEAP_DESC heap_desc {};
      heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
      heap_desc.NumDescriptors =
        resource_ring_t::slot_count * descriptors_per_slot;
      heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
      status = foundation.device()->CreateDescriptorHeap(
        &heap_desc,
        IID_PPV_ARGS(&impl_->descriptor_heap));
      if (FAILED(status)) {
        impl_->fail(status, "hdr_descriptor_heap_create");
      }
      else {
        impl_->descriptor_increment =
          foundation.device()->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        heap_desc.NumDescriptors = resource_ring_t::slot_count;
        heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        status = foundation.device()->CreateDescriptorHeap(
          &heap_desc, IID_PPV_ARGS(&impl_->clear_descriptor_heap));
        if (FAILED(status)) {
          impl_->fail(status, "hdr_clear_descriptor_heap_create");
        }
      }
    }
    for (std::size_t index = 0;
      SUCCEEDED(status) && index < resource_ring_t::slot_count;
      ++index) {
      status = impl_->create_slot(index, d3d11_device);
    }
    if (SUCCEEDED(status)) {
      status = impl_->ring.begin_generation(generation) ?
                 S_OK :
                 E_UNEXPECTED;
      if (FAILED(status)) {
        impl_->fail(status, "hdr_ring_generation");
      }
    }
    if (FAILED(status)) {
      return { false, impl_->failure_hresult, impl_->failure_stage };
    }
    impl_->available = true;
    impl_->failure_stage = "ready";
    return { true, S_OK, "ready" };
  }

  bool
  hdr_analysis_t::available() const {
    return impl_ && impl_->available;
  }

  std::optional<writable_snapshot_t>
  hdr_analysis_t::try_acquire_snapshot() {
    if (!available()) {
      return std::nullopt;
    }
    const auto removed_reason =
      impl_->foundation->device()->GetDeviceRemovedReason();
    if (FAILED(removed_reason)) {
      impl_->fail(removed_reason, "hdr_device_removed");
      return std::nullopt;
    }
    const auto completed =
      impl_->foundation->shared_fence()->GetCompletedValue();
    const auto index =
      impl_->ring.try_acquire(completed);
    if (!index) {
      return std::nullopt;
    }
    auto &slot = impl_->slots[*index];
    slot.generation = impl_->ring.generation();
    return writable_snapshot_t {
      *index,
      slot.generation,
      slot.d3d11_snapshot.Get(),
      slot.d3d11_snapshot_uav.Get(),
      slot.d3d11_pq_snapshot_uav.Get(),
    };
  }

  bool
  hdr_analysis_t::cancel_snapshot(
    const writable_snapshot_t &snapshot) {
    return available() && snapshot.generation == impl_->ring.generation() &&
           impl_->ring.cancel_capture(snapshot.slot);
  }

  bool
  hdr_analysis_t::submit(
    const writable_snapshot_t &snapshot,
    std::uint64_t source_frame) {
    if (!available() || snapshot.slot >= resource_ring_t::slot_count ||
        snapshot.generation !=
          impl_->ring.generation()) {
      return false;
    }
    auto &ring = impl_->ring;
    auto submission_lock = impl_->foundation->lock_submission();
    const auto capture_ready = impl_->foundation->next_fence_value();
    const auto compute_done = impl_->foundation->next_fence_value();
    const auto encode_done = impl_->foundation->next_fence_value();
    if (capture_ready == 0 || compute_done == 0 || encode_done == 0) {
      impl_->fail(E_UNEXPECTED, "hdr_fence_value");
      return false;
    }

    auto status = impl_->d3d11_context4->Signal(
      impl_->d3d11_fence.Get(),
      capture_ready);
    if (SUCCEEDED(status)) {
      // Submit the producer batch before a different API waits for it. This
      // asynchronous flush runs only on analysis submissions, not every frame.
      impl_->d3d11_context4->Flush();
    }
    if (SUCCEEDED(status) &&
        !ring.mark_capture_ready(snapshot.slot, capture_ready)) {
      status = E_UNEXPECTED;
    }
    if (SUCCEEDED(status)) {
      status = impl_->record_commands(snapshot.slot);
    }
    if (SUCCEEDED(status)) {
      status = impl_->foundation->compute_queue()->Wait(
        impl_->capture_fence.Get(),
        capture_ready);
    }
    if (SUCCEEDED(status)) {
      ID3D12CommandList *lists[] {
        impl_->slots[snapshot.slot].command_list.Get(),
      };
      impl_->foundation->compute_queue()->ExecuteCommandLists(1, lists);
      status = impl_->foundation->compute_queue()->Signal(
        impl_->foundation->shared_fence(),
        compute_done);
    }
    if (SUCCEEDED(status) &&
        !ring.mark_compute_queued(snapshot.slot, compute_done, true)) {
      status = E_UNEXPECTED;
    }
    if (SUCCEEDED(status)) {
      status = impl_->foundation->compute_queue()->Signal(
        impl_->foundation->shared_fence(),
        encode_done);
    }
    if (SUCCEEDED(status) &&
        !ring.mark_encoder_queued(snapshot.slot, encode_done)) {
      status = E_UNEXPECTED;
    }
    if (FAILED(status)) {
      submission_lock.unlock();
      (void) impl_->foundation->wait_idle();
      impl_->fail(status, "hdr_submit");
      return false;
    }
    auto &slot = impl_->slots[snapshot.slot];
    slot.source_frame = source_frame;
    slot.generation = snapshot.generation;
    return true;
  }

  std::optional<completed_hdr_result_t>
  hdr_analysis_t::poll() {
    if (!available()) {
      return std::nullopt;
    }
    const auto removed_reason =
      impl_->foundation->device()->GetDeviceRemovedReason();
    if (FAILED(removed_reason)) {
      impl_->fail(removed_reason, "hdr_device_removed");
      return std::nullopt;
    }
    const auto completed =
      impl_->foundation->shared_fence()->GetCompletedValue();
    std::optional<completed_hdr_result_t> newest;
    auto &ring = impl_->ring;
    for (std::size_t index = 0;
      index < resource_ring_t::slot_count;
      ++index) {
      const auto &state = ring.slot(index);
      if (state.state != slot_state_e::encoder_queued ||
          state.compute_done > completed ||
          state.analysis_readback_released) {
        continue;
      }

      auto &slot = impl_->slots[index];
      void *mapped = nullptr;
      const D3D12_RANGE read_range {
        0,
        sizeof(hdr_final_result_t),
      };
      const auto status = slot.readback->Map(0, &read_range, &mapped);
      if (FAILED(status)) {
        impl_->fail(status, "hdr_readback_map");
        return std::nullopt;
      }
      completed_hdr_result_t candidate {
        {},
        slot.source_frame,
        slot.generation,
      };
      std::memcpy(
        &candidate.result,
        mapped,
        sizeof(candidate.result));
      const D3D12_RANGE no_write { 0, 0 };
      slot.readback->Unmap(0, &no_write);
      if (!ring.release_analysis_readback(index, slot.generation)) {
        impl_->fail(E_UNEXPECTED, "hdr_readback_release");
        return std::nullopt;
      }
      if (slot.generation == ring.generation() &&
          (!newest ||
            candidate.source_frame > newest->source_frame)) {
        newest = candidate;
      }
    }
    ring.retire_completed(completed);
    return newest;
  }

  HRESULT
  hdr_analysis_t::failure_hresult() const {
    return impl_ ? impl_->failure_hresult : E_UNEXPECTED;
  }

  std::string_view
  hdr_analysis_t::failure_stage() const {
    return impl_ ? impl_->failure_stage : "destroyed";
  }

  void
  hdr_analysis_t::disable() {
    if (!impl_) {
      return;
    }
    // fail() can clear available while other slots still reference GPU resources.
    if (impl_->foundation && impl_->foundation->available()) {
      (void) impl_->foundation->wait_idle();
    }
    if (impl_->available) {
      (void) poll();
    }
    impl_->available = false;
  }
}  // namespace platf::dxgi::d3d12
