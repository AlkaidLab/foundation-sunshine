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
    release_resources();
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
    std::uint64_t generation,
    bool timing_enabled) {
    release_resources();
    impl_ = std::make_unique<impl_t>();
    if (!foundation.available() || !d3d11_device || !d3d11_context ||
        analysis_width == 0 || analysis_height == 0 ||
        source_width == 0 || source_height == 0 || generation == 0) {
      impl_->fail(E_INVALIDARG, "hdr_input_validation");
      return { false, impl_->failure_hresult, impl_->failure_stage };
    }

    impl_->foundation = &foundation;
    impl_->retirement_device = foundation.device();
    impl_->retirement_queue = foundation.compute_queue();
    impl_->completion_fence = foundation.shared_fence();
    impl_->producer_device = d3d11_device;
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
    if (SUCCEEDED(status) && timing_enabled) {
      D3D12_QUERY_HEAP_DESC desc {};
      desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
      desc.Count = resource_ring_t::slot_count * timing_query_count;
      LARGE_INTEGER qpc_frequency {};
      if (SUCCEEDED(foundation.compute_queue()->GetTimestampFrequency(&impl_->timestamp_frequency)) &&
          impl_->timestamp_frequency != 0 && QueryPerformanceFrequency(&qpc_frequency)) {
        impl_->qpc_frequency = qpc_frequency.QuadPart;
        // Telemetry is optional and must never make a stream unavailable.
        foundation.device()->CreateQueryHeap(&desc, IID_PPV_ARGS(&impl_->timing_queries));
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
    impl_->gpu_resources_exposed = true;
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
    std::uint64_t source_frame,
    bool measure_timing) {
    if (!available() || snapshot.slot >= resource_ring_t::slot_count ||
        snapshot.generation !=
          impl_->ring.generation()) {
      return false;
    }
    auto &ring = impl_->ring;
    auto &submitted_slot = impl_->slots[snapshot.slot];
    submitted_slot.measured = measure_timing && impl_->timing_queries;
    submitted_slot.calibrated = false;
    if (submitted_slot.measured) submitted_slot.submitted_at = std::chrono::steady_clock::now();
    auto submission_lock = impl_->foundation->lock_submission();
    const auto capture_ready = impl_->foundation->next_fence_value();
    const auto compute_done = impl_->foundation->next_fence_value();
    const auto encode_done = impl_->foundation->next_fence_value();
    if (capture_ready == 0 || compute_done == 0 || encode_done == 0) {
      impl_->fail(E_UNEXPECTED, "hdr_fence_value");
      return false;
    }

    impl_->last_capture_value = capture_ready;
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
      if (submitted_slot.measured) {
        submitted_slot.calibrated = SUCCEEDED(impl_->foundation->compute_queue()->GetClockCalibration(
          &submitted_slot.calibration_gpu, &submitted_slot.calibration_cpu));
        LARGE_INTEGER now {};
        QueryPerformanceCounter(&now);
        submitted_slot.submit_cpu = now.QuadPart;
      }
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
      impl_->fail(status, "hdr_submit");
      disable();
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
        slot.measured ? timing_readback_offset + timing_query_count * sizeof(std::uint64_t) : sizeof(hdr_final_result_t),
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
        {},
      };
      std::memcpy(
        &candidate.result,
        mapped,
        sizeof(candidate.result));
      if (slot.measured) {
        std::array<std::uint64_t, timing_query_count> ticks {};
        std::memcpy(ticks.data(), static_cast<const std::byte *>(mapped) + timing_readback_offset, sizeof(ticks));
        if (ticks[0] <= ticks[1] && ticks[1] <= ticks[2] && ticks[2] <= ticks[3]) {
          auto ms = [&](std::uint64_t delta) { return delta * 1000.0 / impl_->timestamp_frequency; };
          completed_hdr_result_t::timing_t timing;
          timing.pass1_ms = ms(ticks[1] - ticks[0]);
          timing.pass2_ms = ms(ticks[2] - ticks[1]);
          timing.readback_ms = ms(ticks[3] - ticks[2]);
          timing.gpu_total_ms = ms(ticks[3] - ticks[0]);
          if (slot.calibrated) {
            const auto delta = (static_cast<long double>(ticks[0]) - slot.calibration_gpu) * 1000.0L / impl_->timestamp_frequency -
                               (static_cast<long double>(slot.submit_cpu) - slot.calibration_cpu) * 1000.0L / impl_->qpc_frequency;
            // Calibration is approximate; do not report a negative queue delay.
            if (delta >= 0) timing.submit_to_start_ms = static_cast<double>(delta);
          }
          timing.poll_latency_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - slot.submitted_at).count();
          candidate.timing = timing;
        }
      }
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

}  // namespace platf::dxgi::d3d12
