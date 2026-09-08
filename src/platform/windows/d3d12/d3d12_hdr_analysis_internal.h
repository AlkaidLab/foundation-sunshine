/**
 * @file src/platform/windows/d3d12/d3d12_hdr_analysis_internal.h
 * @brief Private resources and shader ABI shared by D3D12 analysis implementation files.
 */
#pragma once

#include "d3d12_hdr_analysis.h"

namespace platf::dxgi::d3d12 {
  using Microsoft::WRL::ComPtr;

  namespace hdr_analysis_detail {
    constexpr std::size_t descriptors_per_slot = 8;
    constexpr std::size_t constant_buffer_alignment = 256;

    struct group_result_t {
      float min_maxrgb;
      float max_maxrgb;
      float sum_maxrgb;
      float sum_maxrgb_pq;
      std::uint32_t pixel_count;
    };
    static_assert(sizeof(group_result_t) == 20);

    struct analysis_params_t {
      std::uint32_t analysis_width;
      std::uint32_t analysis_height;
      std::uint32_t source_width;
      std::uint32_t source_height;
      std::uint32_t input_has_cell_statistics;
      float max_analysis_nits;
      std::uint32_t padding[2];
    };
    static_assert(sizeof(analysis_params_t) == 32);

    struct reduce_params_t {
      std::uint32_t num_groups;
      std::uint32_t padding[3];
    };
    static_assert(sizeof(reduce_params_t) == 16);

    inline D3D12_RESOURCE_DESC
    buffer_desc(
      std::uint64_t size,
      D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
      D3D12_RESOURCE_DESC desc {};
      desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      desc.Width = size;
      desc.Height = 1;
      desc.DepthOrArraySize = 1;
      desc.MipLevels = 1;
      desc.Format = DXGI_FORMAT_UNKNOWN;
      desc.SampleDesc.Count = 1;
      desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      desc.Flags = flags;
      return desc;
    }

    inline D3D12_CPU_DESCRIPTOR_HANDLE
    cpu_handle(
      ID3D12DescriptorHeap *heap,
      UINT increment,
      std::size_t index) {
      auto handle = heap->GetCPUDescriptorHandleForHeapStart();
      handle.ptr += increment * index;
      return handle;
    }

    inline D3D12_GPU_DESCRIPTOR_HANDLE
    gpu_handle(
      ID3D12DescriptorHeap *heap,
      UINT increment,
      std::size_t index) {
      auto handle = heap->GetGPUDescriptorHandleForHeapStart();
      handle.ptr += increment * index;
      return handle;
    }
  }  // namespace hdr_analysis_detail

  struct hdr_analysis_t::impl_t {
    struct slot_t {
      ComPtr<ID3D12Resource> snapshot;
      ComPtr<ID3D11Texture2D> d3d11_snapshot;
      ComPtr<ID3D11UnorderedAccessView> d3d11_snapshot_uav;
      ComPtr<ID3D12Resource> pq_snapshot;
      ComPtr<ID3D11Texture2D> d3d11_pq_snapshot;
      ComPtr<ID3D11UnorderedAccessView> d3d11_pq_snapshot_uav;
      ComPtr<ID3D12Resource> group_results;
      ComPtr<ID3D12Resource> histogram;
      ComPtr<ID3D12Resource> final_result;
      ComPtr<ID3D12Resource> readback;
      ComPtr<ID3D12Resource> constants;
      ComPtr<ID3D12CommandAllocator> allocator;
      ComPtr<ID3D12GraphicsCommandList> command_list;
      std::uint64_t source_frame = 0;
      std::uint64_t generation = 0;
    };

    device_t *foundation = nullptr;
    // Strong references keep retirement independent of the display's lifetime.
    ComPtr<ID3D12Device> retirement_device;
    ComPtr<ID3D12CommandQueue> retirement_queue;
    ComPtr<ID3D12Fence> completion_fence;
    ComPtr<ID3D12Fence> retirement_fence;
    ComPtr<ID3D11Device> producer_device;
    ComPtr<ID3D11DeviceContext4> d3d11_context4;
    ComPtr<ID3D11Fence> d3d11_fence;
    // Each fence has one signaling queue. A later D3D11 signal must never
    // make an earlier D3D12 result appear complete before compute finishes.
    ComPtr<ID3D12Fence> capture_fence;
    ComPtr<ID3D12RootSignature> root_signature;
    ComPtr<ID3D12PipelineState> pass1_pipeline;
    ComPtr<ID3D12PipelineState> pass2_pipeline;
    ComPtr<ID3D12DescriptorHeap> descriptor_heap;
    ComPtr<ID3D12DescriptorHeap> clear_descriptor_heap;
    std::array<slot_t, resource_ring_t::slot_count> slots;
    // Slots describe these resources, not every analyzer on the shared device.
    resource_ring_t ring;
    std::uint32_t analysis_width = 0;
    std::uint32_t analysis_height = 0;
    std::uint32_t source_width = 0;
    std::uint32_t source_height = 0;
    std::uint32_t num_groups = 0;
    float max_analysis_nits = 10000.0f;
    UINT descriptor_increment = 0;
    HRESULT failure_hresult = S_OK;
    std::string_view failure_stage = "none";
    bool available = false;
    bool gpu_resources_exposed = false;
    bool retirement_started = false;
    bool producer_retirement_signaled = false;
    bool compute_retirement_signaled = false;
    std::uint64_t last_capture_value = 0;
    std::uint64_t capture_retirement_value = 0;

    HRESULT
    fail(HRESULT status, std::string_view stage) {
      failure_hresult = status;
      failure_stage = stage;
      available = false;
      return status;
    }

    HRESULT
    create_pipeline();
    HRESULT
    create_shared_fence_view(ID3D11Device *d3d11_device);
    HRESULT
    create_slot(std::size_t slot_index, ID3D11Device *d3d11_device);
    HRESULT
    record_commands(std::size_t slot_index);

    bool
    retirement_complete(bool can_submit_producer);

    static unsigned __stdcall
    retirement_worker(void *raw);
  };
}  // namespace platf::dxgi::d3d12
