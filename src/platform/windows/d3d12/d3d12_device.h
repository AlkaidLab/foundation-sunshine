/**
 * @file src/platform/windows/d3d12/d3d12_device.h
 * @brief D3D12 device, compute queue, shared fence, and ring bootstrap.
 */
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "d3d12_resource_ring.h"
#include "src/platform/windows/video_backend.h"

namespace platf::dxgi::d3d12 {
  struct capabilities_t {
    D3D12_SHARED_RESOURCE_COMPATIBILITY_TIER shared_resource_tier =
      D3D12_SHARED_RESOURCE_COMPATIBILITY_TIER_0;
    bool shared_fence = false;
    bool rgba16f_bridge = false;
    bool nv12_encoder_surface = false;
    bool p010_encoder_surface = false;

    [[nodiscard]] bool
    has_viable_topology() const {
      return rgba16f_bridge || nv12_encoder_surface ||
             p010_encoder_surface;
    }
  };

  struct init_result_t {
    bool success = false;
    video_backend::fallback_reason_e reason =
      video_backend::fallback_reason_e::none;
    HRESULT hresult = S_OK;
    std::string_view stage = "none";
    capabilities_t capabilities;
  };

  class device_t {
  public:
    device_t() = default;
    device_t(const device_t &) = delete;
    device_t &
    operator=(const device_t &) = delete;
    ~device_t();

    [[nodiscard]] init_result_t
    initialize(
      IDXGIAdapter1 *adapter,
      ID3D11Device *d3d11_device,
      ID3D11DeviceContext *d3d11_context);

    [[nodiscard]] bool
    available() const;

    [[nodiscard]] ID3D12Device *
    device() const;

    [[nodiscard]] ID3D12CommandQueue *
    compute_queue() const;

    [[nodiscard]] ID3D12Fence *
    shared_fence() const;

    [[nodiscard]] const capabilities_t &
    capabilities() const;

    [[nodiscard]] std::uint64_t
    next_fence_value();

    [[nodiscard]] resource_ring_t &
    resource_ring();

    void
    drain();

    [[nodiscard]] HRESULT
    wait_idle();

  private:
    [[nodiscard]] HRESULT
    probe_features();

    [[nodiscard]] HRESULT
    create_queue_fence_and_allocators();

    [[nodiscard]] HRESULT
    self_test_shared_fence(
      ID3D11Device *d3d11_device,
      ID3D11DeviceContext *d3d11_context);

    [[nodiscard]] HRESULT
    self_test_shared_texture(
      ID3D11Device *d3d11_device,
      ID3D11DeviceContext *d3d11_context,
      DXGI_FORMAT format);

    void
    reset();

    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> compute_queue_;
    Microsoft::WRL::ComPtr<ID3D12Fence> shared_fence_;
    std::array<
      Microsoft::WRL::ComPtr<ID3D12CommandAllocator>,
      resource_ring_t::slot_count>
      command_allocators_;
    resource_ring_t resource_ring_;
    capabilities_t capabilities_;
    std::uint64_t last_fence_value_ = 0;
    std::string_view self_test_stage_ = "none";
    bool available_ = false;
  };
}  // namespace platf::dxgi::d3d12
