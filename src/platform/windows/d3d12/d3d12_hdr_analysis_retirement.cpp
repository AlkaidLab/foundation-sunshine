/**
 * @file src/platform/windows/d3d12/d3d12_hdr_analysis_retirement.cpp
 * @brief Retain analyzer resources until both GPU producers have stopped using them.
 */
#include "d3d12_hdr_analysis_internal.h"

#include <atomic>
#include <chrono>
#include <limits>
#include <process.h>

namespace platf::dxgi::d3d12 {
  namespace {
    std::atomic<std::uint32_t> pending_retirements { 0 };
    constexpr auto retirement_timeout = std::chrono::seconds(2);
  }  // namespace

  bool
  hdr_analysis_t::impl_t::retirement_complete(bool can_submit_producer) {
    if (!gpu_resources_exposed) return true;

    const bool producer_removed = FAILED(producer_device->GetDeviceRemovedReason());
    const bool compute_removed = FAILED(retirement_device->GetDeviceRemovedReason());
    if (can_submit_producer && !producer_removed && !producer_retirement_signaled && capture_retirement_value != 0) {
      producer_retirement_signaled = SUCCEEDED(d3d11_context4->Signal(
        d3d11_fence.Get(), capture_retirement_value));
      if (producer_retirement_signaled) d3d11_context4->Flush();
    }
    if (!compute_removed && !compute_retirement_signaled) {
      // A private fence avoids advancing another analyzer's completion values.
      compute_retirement_signaled = SUCCEEDED(retirement_queue->Signal(retirement_fence.Get(), 1));
    }

    const auto producer_completed = d3d11_fence->GetCompletedValue();
    const auto compute_completed = retirement_fence->GetCompletedValue();
    const auto removed_value = std::numeric_limits<std::uint64_t>::max();
    const bool producer_done = producer_removed ||
                               (producer_retirement_signaled && producer_completed != removed_value &&
                                 producer_completed >= capture_retirement_value);
    const bool compute_done = compute_removed ||
                              (compute_retirement_signaled && compute_completed != removed_value && compute_completed >= 1);
    return producer_done && compute_done;
  }

  void
  hdr_analysis_t::disable() {
    if (!impl_ || impl_->retirement_started) return;
    impl_->available = false;
    impl_->retirement_started = true;
    if (impl_->last_capture_value < std::numeric_limits<std::uint64_t>::max() - 1) {
      impl_->capture_retirement_value = impl_->last_capture_value + 1;
    }
    const auto deadline = std::chrono::steady_clock::now() + retirement_timeout;
    while (!impl_->retirement_complete(true) && std::chrono::steady_clock::now() < deadline) {
      Sleep(1);
    }
  }

  unsigned __stdcall hdr_analysis_t::impl_t::retirement_worker(void *raw) {
    std::unique_ptr<impl_t> retired(static_cast<impl_t *>(raw));
    // This state owns both devices, the queue, fences and resources. It never
    // dereferences foundation, which may already have been destroyed, or submits
    // commands to a D3D11 immediate context that the next session may be using.
    while (!retired->retirement_complete(false)) Sleep(10);
    retired.reset();
    --pending_retirements;
    return 0;
  }

  void
  hdr_analysis_t::release_resources() {
    disable();
    if (!impl_ || impl_->retirement_complete(false)) return;

    // A timeout is not proof that the GPU is idle. Transfer ownership instead
    // of letting the caller's destructor free resources still in use.
    auto *retired = impl_.release();
    ++pending_retirements;
    const auto worker = _beginthreadex(nullptr, 0, &impl_t::retirement_worker, retired, 0, nullptr);
    if (worker) {
      CloseHandle(reinterpret_cast<HANDLE>(worker));
    }
    else {
      // On thread-allocation failure, retain the resources until process exit.
      // Freeing unknown in-flight allocations is unsafe even in this rare case.
      OutputDebugStringA("Sunshine: unable to start HDR resource retirement worker; retaining GPU resources\n");
    }
  }

  std::uint32_t
  hdr_analysis_t::pending_resource_retirements() {
    return pending_retirements.load();
  }
}  // namespace platf::dxgi::d3d12
