/**
 * @file src/platform/windows/d3d12/d3d12_hdr_analysis_resources.cpp
 * @brief Shared textures, descriptors and buffers for D3D12 HDR analysis.
 */
#include "d3d12_hdr_analysis_internal.h"

#include <cstring>
#include <limits>

namespace platf::dxgi::d3d12 {
  using namespace hdr_analysis_detail;

  HRESULT
  hdr_analysis_t::impl_t::create_shared_fence_view(
    ID3D11Device *d3d11_device) {
    auto status = foundation->device()->CreateFence(
      0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&capture_fence));
    if (FAILED(status)) {
      return fail(status, "hdr_capture_fence_create");
    }
    HANDLE shared_handle = nullptr;
    status = foundation->device()->CreateSharedHandle(
      capture_fence.Get(),
      nullptr,
      GENERIC_ALL,
      nullptr,
      &shared_handle);
    if (FAILED(status)) {
      return fail(status, "hdr_shared_fence_handle");
    }

    ComPtr<ID3D11Device5> device5;
    status = d3d11_device->QueryInterface(IID_PPV_ARGS(&device5));
    if (SUCCEEDED(status)) {
      status = device5->OpenSharedFence(
        shared_handle,
        IID_PPV_ARGS(&d3d11_fence));
    }
    CloseHandle(shared_handle);
    return FAILED(status) ?
             fail(status, "hdr_d3d11_open_shared_fence") :
             S_OK;
  }

  HRESULT
  hdr_analysis_t::impl_t::create_slot(
    std::size_t slot_index,
    ID3D11Device *d3d11_device) {
    auto &slot = slots[slot_index];
    D3D12_HEAP_PROPERTIES default_heap {};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    const auto create_snapshot = [&](DXGI_FORMAT format,
                                   ComPtr<ID3D12Resource> &resource,
                                   ComPtr<ID3D11Texture2D> &d3d11_texture,
                                   ComPtr<ID3D11UnorderedAccessView> &d3d11_uav) -> HRESULT {
      D3D12_RESOURCE_DESC snapshot_desc {};
      snapshot_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
      snapshot_desc.Width = analysis_width;
      snapshot_desc.Height = analysis_height;
      snapshot_desc.DepthOrArraySize = 1;
      snapshot_desc.MipLevels = 1;
      snapshot_desc.Format = format;
      snapshot_desc.SampleDesc.Count = 1;
      snapshot_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
      // ALLOW_UNORDERED_ACCESS so the D3D11 conversion shader can write the
      // cell-statistics snapshot directly into this shared resource.
      // ALLOW_SIMULTANEOUS_ACCESS keeps it usable from both devices while it
      // stays in the COMMON state; the shared fence orders the two accesses.
      snapshot_desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
        D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS);
      auto status = foundation->device()->CreateCommittedResource(
        &default_heap,
        D3D12_HEAP_FLAG_SHARED,
        &snapshot_desc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(&resource));
      if (FAILED(status)) {
        return fail(status, "hdr_snapshot_create");
      }

      HANDLE shared_handle = nullptr;
      status = foundation->device()->CreateSharedHandle(
        resource.Get(),
        nullptr,
        GENERIC_ALL,
        nullptr,
        &shared_handle);
      ComPtr<ID3D11Device1> device1;
      if (SUCCEEDED(status)) {
        status = d3d11_device->QueryInterface(IID_PPV_ARGS(&device1));
      }
      if (SUCCEEDED(status)) {
        status = device1->OpenSharedResource1(
          shared_handle,
          IID_PPV_ARGS(&d3d11_texture));
      }
      if (shared_handle) {
        CloseHandle(shared_handle);
      }
      if (FAILED(status)) {
        return fail(status, "hdr_snapshot_open_d3d11");
      }

      D3D11_UNORDERED_ACCESS_VIEW_DESC snapshot_uav_desc {};
      snapshot_uav_desc.Format = format;
      snapshot_uav_desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
      status = d3d11_device->CreateUnorderedAccessView(
        d3d11_texture.Get(),
        &snapshot_uav_desc,
        &d3d11_uav);
      if (FAILED(status)) {
        return fail(status, "hdr_snapshot_uav_create");
      }
      return S_OK;
    };
    auto status = create_snapshot(DXGI_FORMAT_R16G16B16A16_FLOAT,
      slot.snapshot, slot.d3d11_snapshot, slot.d3d11_snapshot_uav);
    if (SUCCEEDED(status)) {
      status = create_snapshot(DXGI_FORMAT_R16_FLOAT,
        slot.pq_snapshot, slot.d3d11_pq_snapshot, slot.d3d11_pq_snapshot_uav);
    }
    if (FAILED(status)) {
      return status;
    }
    auto group_desc = buffer_desc(
      static_cast<std::uint64_t>(num_groups) * sizeof(group_result_t),
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    status = foundation->device()->CreateCommittedResource(
      &default_heap,
      D3D12_HEAP_FLAG_NONE,
      &group_desc,
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      nullptr,
      IID_PPV_ARGS(&slot.group_results));
    if (FAILED(status)) {
      return fail(status, "hdr_group_buffer_create");
    }

    auto histogram_desc = buffer_desc(
      hdr_histogram_bins * sizeof(std::uint32_t),
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    status = foundation->device()->CreateCommittedResource(
      &default_heap,
      D3D12_HEAP_FLAG_NONE,
      &histogram_desc,
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      nullptr,
      IID_PPV_ARGS(&slot.histogram));
    if (FAILED(status)) {
      return fail(status, "hdr_histogram_create");
    }

    auto final_desc = buffer_desc(
      sizeof(hdr_final_result_t),
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    status = foundation->device()->CreateCommittedResource(
      &default_heap,
      D3D12_HEAP_FLAG_NONE,
      &final_desc,
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      nullptr,
      IID_PPV_ARGS(&slot.final_result));
    if (FAILED(status)) {
      return fail(status, "hdr_final_buffer_create");
    }

    D3D12_HEAP_PROPERTIES readback_heap {};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    auto readback_desc = buffer_desc(sizeof(hdr_final_result_t));
    status = foundation->device()->CreateCommittedResource(
      &readback_heap,
      D3D12_HEAP_FLAG_NONE,
      &readback_desc,
      D3D12_RESOURCE_STATE_COPY_DEST,
      nullptr,
      IID_PPV_ARGS(&slot.readback));
    if (FAILED(status)) {
      return fail(status, "hdr_readback_create");
    }

    D3D12_HEAP_PROPERTIES upload_heap {};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    auto constant_desc = buffer_desc(constant_buffer_alignment * 3);
    status = foundation->device()->CreateCommittedResource(
      &upload_heap,
      D3D12_HEAP_FLAG_NONE,
      &constant_desc,
      D3D12_RESOURCE_STATE_GENERIC_READ,
      nullptr,
      IID_PPV_ARGS(&slot.constants));
    if (FAILED(status)) {
      return fail(status, "hdr_constants_create");
    }
    void *mapped = nullptr;
    D3D12_RANGE no_read { 0, 0 };
    status = slot.constants->Map(0, &no_read, &mapped);
    if (FAILED(status)) {
      return fail(status, "hdr_constants_map");
    }
    const analysis_params_t analysis_params {
      analysis_width,
      analysis_height,
      source_width,
      source_height,
      1,
      max_analysis_nits,
      {},
    };
    const reduce_params_t reduce_params { num_groups, {} };
    // Snapshot pixels already include the pre-encode transform. The common
    // shader still declares b3 for its full-frame branch, so bind a disabled
    // transform rather than leaving a root argument uninitialized.
    std::memset(mapped, 0, constant_buffer_alignment * 3);
    std::memcpy(mapped, &analysis_params, sizeof(analysis_params));
    std::memcpy(
      static_cast<std::byte *>(mapped) + constant_buffer_alignment,
      &reduce_params,
      sizeof(reduce_params));
    slot.constants->Unmap(0, nullptr);

    status = foundation->device()->CreateCommandAllocator(
      D3D12_COMMAND_LIST_TYPE_COMPUTE,
      IID_PPV_ARGS(&slot.allocator));
    if (SUCCEEDED(status)) {
      status = foundation->device()->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_COMPUTE,
        slot.allocator.Get(),
        pass1_pipeline.Get(),
        IID_PPV_ARGS(&slot.command_list));
    }
    if (SUCCEEDED(status)) {
      status = slot.command_list->Close();
    }
    if (FAILED(status)) {
      return fail(status, "hdr_command_list_create");
    }

    const auto base = slot_index * descriptors_per_slot;
    D3D12_SHADER_RESOURCE_VIEW_DESC snapshot_srv {};
    snapshot_srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    snapshot_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    snapshot_srv.Shader4ComponentMapping =
      D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    snapshot_srv.Texture2D.MipLevels = 1;
    foundation->device()->CreateShaderResourceView(
      slot.snapshot.Get(),
      &snapshot_srv,
      cpu_handle(descriptor_heap.Get(), descriptor_increment, base));
    snapshot_srv.Format = DXGI_FORMAT_R16_FLOAT;
    foundation->device()->CreateShaderResourceView(
      slot.pq_snapshot.Get(), &snapshot_srv,
      cpu_handle(descriptor_heap.Get(), descriptor_increment, base + 1));

    D3D12_UNORDERED_ACCESS_VIEW_DESC group_uav {};
    group_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    group_uav.Format = DXGI_FORMAT_UNKNOWN;
    group_uav.Buffer.NumElements = num_groups;
    group_uav.Buffer.StructureByteStride = sizeof(group_result_t);
    foundation->device()->CreateUnorderedAccessView(
      slot.group_results.Get(),
      nullptr,
      &group_uav,
      cpu_handle(descriptor_heap.Get(), descriptor_increment, base + 2));

    D3D12_UNORDERED_ACCESS_VIEW_DESC histogram_uav {};
    histogram_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    histogram_uav.Format = DXGI_FORMAT_R32_UINT;
    histogram_uav.Buffer.NumElements = hdr_histogram_bins;
    // ClearUnorderedAccessViewUint requires a CPU-readable descriptor in a
    // non-shader-visible heap, alongside the matching GPU-visible view.
    foundation->device()->CreateUnorderedAccessView(
      slot.histogram.Get(), nullptr, &histogram_uav,
      cpu_handle(clear_descriptor_heap.Get(), descriptor_increment, slot_index));
    foundation->device()->CreateUnorderedAccessView(
      slot.histogram.Get(),
      nullptr,
      &histogram_uav,
      cpu_handle(descriptor_heap.Get(), descriptor_increment, base + 3));

    D3D12_SHADER_RESOURCE_VIEW_DESC group_srv {};
    group_srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    group_srv.Format = DXGI_FORMAT_UNKNOWN;
    group_srv.Shader4ComponentMapping =
      D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    group_srv.Buffer.NumElements = num_groups;
    group_srv.Buffer.StructureByteStride = sizeof(group_result_t);
    foundation->device()->CreateShaderResourceView(
      slot.group_results.Get(),
      &group_srv,
      cpu_handle(descriptor_heap.Get(), descriptor_increment, base + 4));
    // Pass 2 only reads t0, but its table shares the two-SRV root layout.
    foundation->device()->CreateShaderResourceView(
      nullptr, &snapshot_srv,
      cpu_handle(descriptor_heap.Get(), descriptor_increment, base + 5));

    D3D12_UNORDERED_ACCESS_VIEW_DESC final_uav {};
    final_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    final_uav.Format = DXGI_FORMAT_UNKNOWN;
    final_uav.Buffer.NumElements = 1;
    final_uav.Buffer.StructureByteStride = sizeof(hdr_final_result_t);
    foundation->device()->CreateUnorderedAccessView(
      slot.final_result.Get(),
      nullptr,
      &final_uav,
      cpu_handle(descriptor_heap.Get(), descriptor_increment, base + 6));
    foundation->device()->CreateUnorderedAccessView(
      slot.histogram.Get(),
      nullptr,
      &histogram_uav,
      cpu_handle(descriptor_heap.Get(), descriptor_increment, base + 7));
    return S_OK;
  }

}  // namespace platf::dxgi::d3d12
