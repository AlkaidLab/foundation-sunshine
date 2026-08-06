/**
 * @file src/platform/windows/d3d12/d3d12_hdr_analysis.cpp
 * @brief Experimental D3D12 two-pass HDR luminance analysis backend.
 */
#include "d3d12_hdr_analysis.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include <d3dcompiler.h>
#include <wrl/client.h>

#include "generated/windows/d3d12/d3d12_hdr_shaders.h"

namespace platf::dxgi::d3d12 {
  using Microsoft::WRL::ComPtr;

  namespace {
    constexpr std::size_t descriptors_per_slot = 6;
    constexpr std::size_t constant_buffer_alignment = 256;

    struct group_result_t {
      float min_maxrgb;
      float max_maxrgb;
      float sum_maxrgb;
      std::uint32_t pixel_count;
    };
    static_assert(sizeof(group_result_t) == 16);

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

    D3D12_RESOURCE_DESC
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

    D3D12_CPU_DESCRIPTOR_HANDLE
    cpu_handle(
      ID3D12DescriptorHeap *heap,
      UINT increment,
      std::size_t index) {
      auto handle = heap->GetCPUDescriptorHandleForHeapStart();
      handle.ptr += increment * index;
      return handle;
    }

    D3D12_GPU_DESCRIPTOR_HANDLE
    gpu_handle(
      ID3D12DescriptorHeap *heap,
      UINT increment,
      std::size_t index) {
      auto handle = heap->GetGPUDescriptorHandleForHeapStart();
      handle.ptr += increment * index;
      return handle;
    }
  }  // namespace

  struct hdr_analysis_t::impl_t {
    struct slot_t {
      ComPtr<ID3D12Resource> snapshot;
      ComPtr<ID3D11Texture2D> d3d11_snapshot;
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
    ComPtr<ID3D11DeviceContext4> d3d11_context4;
    ComPtr<ID3D11Fence> d3d11_fence;
    ComPtr<ID3D12RootSignature> root_signature;
    ComPtr<ID3D12PipelineState> pass1_pipeline;
    ComPtr<ID3D12PipelineState> pass2_pipeline;
    ComPtr<ID3D12DescriptorHeap> descriptor_heap;
    std::array<slot_t, resource_ring_t::slot_count> slots;
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

    HRESULT
    fail(HRESULT status, std::string_view stage) {
      failure_hresult = status;
      failure_stage = stage;
      available = false;
      return status;
    }

    HRESULT
    create_pipeline() {
      D3D12_DESCRIPTOR_RANGE ranges[2] {};
      ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
      ranges[0].NumDescriptors = 1;
      ranges[0].BaseShaderRegister = 0;
      ranges[0].OffsetInDescriptorsFromTableStart =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
      ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
      ranges[1].NumDescriptors = 2;
      ranges[1].BaseShaderRegister = 0;
      ranges[1].OffsetInDescriptorsFromTableStart =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

      D3D12_ROOT_PARAMETER parameters[3] {};
      parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      parameters[0].Descriptor.ShaderRegister = 0;
      parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      parameters[1].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      parameters[1].DescriptorTable.NumDescriptorRanges = 1;
      parameters[1].DescriptorTable.pDescriptorRanges = &ranges[0];
      parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      parameters[2].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      parameters[2].DescriptorTable.NumDescriptorRanges = 1;
      parameters[2].DescriptorTable.pDescriptorRanges = &ranges[1];
      parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

      D3D12_ROOT_SIGNATURE_DESC signature_desc {};
      signature_desc.NumParameters = 3;
      signature_desc.pParameters = parameters;
      ComPtr<ID3DBlob> signature;
      ComPtr<ID3DBlob> errors;
      auto status = D3D12SerializeRootSignature(
        &signature_desc,
        D3D_ROOT_SIGNATURE_VERSION_1,
        &signature,
        &errors);
      if (FAILED(status)) {
        return fail(status, "hdr_root_signature_serialize");
      }
      status = foundation->device()->CreateRootSignature(
        0,
        signature->GetBufferPointer(),
        signature->GetBufferSize(),
        IID_PPV_ARGS(&root_signature));
      if (FAILED(status)) {
        return fail(status, "hdr_root_signature_create");
      }

      D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc {};
      pipeline_desc.pRootSignature = root_signature.Get();
      pipeline_desc.CS = {
        shaders::hdr_luminance_analysis_cs_dxil,
        shaders::hdr_luminance_analysis_cs_dxil_size,
      };
      status = foundation->device()->CreateComputePipelineState(
        &pipeline_desc,
        IID_PPV_ARGS(&pass1_pipeline));
      if (FAILED(status)) {
        return fail(status, "hdr_pass1_pipeline_create");
      }
      pipeline_desc.CS = {
        shaders::hdr_luminance_reduce_cs_dxil,
        shaders::hdr_luminance_reduce_cs_dxil_size,
      };
      status = foundation->device()->CreateComputePipelineState(
        &pipeline_desc,
        IID_PPV_ARGS(&pass2_pipeline));
      return FAILED(status) ?
               fail(status, "hdr_pass2_pipeline_create") :
               S_OK;
    }

    HRESULT
    create_shared_fence_view(
      ID3D11Device *d3d11_device) {
      HANDLE shared_handle = nullptr;
      auto status = foundation->device()->CreateSharedHandle(
        foundation->shared_fence(),
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
    create_slot(
      std::size_t slot_index,
      ID3D11Device *d3d11_device) {
      auto &slot = slots[slot_index];
      D3D12_HEAP_PROPERTIES default_heap {};
      default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
      D3D12_RESOURCE_DESC snapshot_desc {};
      snapshot_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
      snapshot_desc.Width = analysis_width;
      snapshot_desc.Height = analysis_height;
      snapshot_desc.DepthOrArraySize = 1;
      snapshot_desc.MipLevels = 1;
      snapshot_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
      snapshot_desc.SampleDesc.Count = 1;
      snapshot_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
      snapshot_desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
        D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS);
      auto status = foundation->device()->CreateCommittedResource(
        &default_heap,
        D3D12_HEAP_FLAG_SHARED,
        &snapshot_desc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(&slot.snapshot));
      if (FAILED(status)) {
        return fail(status, "hdr_snapshot_create");
      }

      HANDLE shared_handle = nullptr;
      status = foundation->device()->CreateSharedHandle(
        slot.snapshot.Get(),
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
          IID_PPV_ARGS(&slot.d3d11_snapshot));
      }
      if (shared_handle) {
        CloseHandle(shared_handle);
      }
      if (FAILED(status)) {
        return fail(status, "hdr_snapshot_open_d3d11");
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
      auto constant_desc = buffer_desc(constant_buffer_alignment * 2);
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

      D3D12_UNORDERED_ACCESS_VIEW_DESC group_uav {};
      group_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
      group_uav.Format = DXGI_FORMAT_UNKNOWN;
      group_uav.Buffer.NumElements = num_groups;
      group_uav.Buffer.StructureByteStride = sizeof(group_result_t);
      foundation->device()->CreateUnorderedAccessView(
        slot.group_results.Get(),
        nullptr,
        &group_uav,
        cpu_handle(descriptor_heap.Get(), descriptor_increment, base + 1));

      D3D12_UNORDERED_ACCESS_VIEW_DESC histogram_uav {};
      histogram_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
      histogram_uav.Format = DXGI_FORMAT_R32_UINT;
      histogram_uav.Buffer.NumElements = hdr_histogram_bins;
      foundation->device()->CreateUnorderedAccessView(
        slot.histogram.Get(),
        nullptr,
        &histogram_uav,
        cpu_handle(descriptor_heap.Get(), descriptor_increment, base + 2));

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
        cpu_handle(descriptor_heap.Get(), descriptor_increment, base + 3));

      D3D12_UNORDERED_ACCESS_VIEW_DESC final_uav {};
      final_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
      final_uav.Format = DXGI_FORMAT_UNKNOWN;
      final_uav.Buffer.NumElements = 1;
      final_uav.Buffer.StructureByteStride = sizeof(hdr_final_result_t);
      foundation->device()->CreateUnorderedAccessView(
        slot.final_result.Get(),
        nullptr,
        &final_uav,
        cpu_handle(descriptor_heap.Get(), descriptor_increment, base + 4));
      foundation->device()->CreateUnorderedAccessView(
        slot.histogram.Get(),
        nullptr,
        &histogram_uav,
        cpu_handle(descriptor_heap.Get(), descriptor_increment, base + 5));
      return S_OK;
    }

    HRESULT
    record_commands(std::size_t slot_index) {
      auto &slot = slots[slot_index];
      auto status = slot.allocator->Reset();
      if (SUCCEEDED(status)) {
        status = slot.command_list->Reset(
          slot.allocator.Get(),
          pass1_pipeline.Get());
      }
      if (FAILED(status)) {
        return fail(status, "hdr_command_list_reset");
      }

      auto *list = slot.command_list.Get();
      ID3D12DescriptorHeap *heaps[] { descriptor_heap.Get() };
      list->SetDescriptorHeaps(1, heaps);
      list->SetComputeRootSignature(root_signature.Get());
      const auto base = slot_index * descriptors_per_slot;
      D3D12_RESOURCE_BARRIER snapshot_to_srv {};
      snapshot_to_srv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      snapshot_to_srv.Transition.pResource = slot.snapshot.Get();
      snapshot_to_srv.Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      snapshot_to_srv.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
      snapshot_to_srv.Transition.StateAfter =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
      list->ResourceBarrier(1, &snapshot_to_srv);

      const UINT clear_values[4] {};
      list->ClearUnorderedAccessViewUint(
        gpu_handle(descriptor_heap.Get(), descriptor_increment, base + 2),
        cpu_handle(descriptor_heap.Get(), descriptor_increment, base + 2),
        slot.histogram.Get(),
        clear_values,
        0,
        nullptr);
      D3D12_RESOURCE_BARRIER histogram_clear_barrier {};
      histogram_clear_barrier.Type =
        D3D12_RESOURCE_BARRIER_TYPE_UAV;
      histogram_clear_barrier.UAV.pResource =
        slot.histogram.Get();
      list->ResourceBarrier(1, &histogram_clear_barrier);

      list->SetPipelineState(pass1_pipeline.Get());
      list->SetComputeRootConstantBufferView(
        0,
        slot.constants->GetGPUVirtualAddress());
      list->SetComputeRootDescriptorTable(
        1,
        gpu_handle(descriptor_heap.Get(), descriptor_increment, base));
      list->SetComputeRootDescriptorTable(
        2,
        gpu_handle(descriptor_heap.Get(), descriptor_increment, base + 1));
      list->Dispatch(
        (analysis_width + 15) / 16,
        (analysis_height + 15) / 16,
        1);

      D3D12_RESOURCE_BARRIER pass1_barriers[2] {};
      pass1_barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
      pass1_barriers[0].UAV.pResource = slot.histogram.Get();
      pass1_barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      pass1_barriers[1].Transition.pResource = slot.group_results.Get();
      pass1_barriers[1].Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      pass1_barriers[1].Transition.StateBefore =
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
      pass1_barriers[1].Transition.StateAfter =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
      list->ResourceBarrier(2, pass1_barriers);

      list->SetPipelineState(pass2_pipeline.Get());
      list->SetComputeRootConstantBufferView(
        0,
        slot.constants->GetGPUVirtualAddress() +
          constant_buffer_alignment);
      list->SetComputeRootDescriptorTable(
        1,
        gpu_handle(descriptor_heap.Get(), descriptor_increment, base + 3));
      list->SetComputeRootDescriptorTable(
        2,
        gpu_handle(descriptor_heap.Get(), descriptor_increment, base + 4));
      list->Dispatch(1, 1, 1);

      D3D12_RESOURCE_BARRIER final_barrier {};
      final_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      final_barrier.Transition.pResource = slot.final_result.Get();
      final_barrier.Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      final_barrier.Transition.StateBefore =
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
      final_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
      list->ResourceBarrier(1, &final_barrier);
      list->CopyBufferRegion(
        slot.readback.Get(),
        0,
        slot.final_result.Get(),
        0,
        sizeof(hdr_final_result_t));

      D3D12_RESOURCE_BARRIER restore[3] {};
      restore[0] = final_barrier;
      std::swap(
        restore[0].Transition.StateBefore,
        restore[0].Transition.StateAfter);
      restore[1] = pass1_barriers[1];
      std::swap(
        restore[1].Transition.StateBefore,
        restore[1].Transition.StateAfter);
      restore[2] = snapshot_to_srv;
      std::swap(
        restore[2].Transition.StateBefore,
        restore[2].Transition.StateAfter);
      list->ResourceBarrier(3, restore);
      status = list->Close();
      return FAILED(status) ?
               fail(status, "hdr_command_list_close") :
               S_OK;
    }
  };

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
      }
    }
    for (std::size_t index = 0;
      SUCCEEDED(status) && index < resource_ring_t::slot_count;
      ++index) {
      status = impl_->create_slot(index, d3d11_device);
    }
    if (SUCCEEDED(status) &&
        foundation.resource_ring().generation() != generation) {
      status = foundation.resource_ring().begin_generation(generation) ?
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
      impl_->foundation->resource_ring().try_acquire(completed);
    if (!index) {
      return std::nullopt;
    }
    auto &slot = impl_->slots[*index];
    slot.generation = impl_->foundation->resource_ring().generation();
    return writable_snapshot_t {
      *index,
      slot.generation,
      slot.d3d11_snapshot.Get(),
    };
  }

  bool
  hdr_analysis_t::cancel_snapshot(
    const writable_snapshot_t &snapshot) {
    return available() && snapshot.generation == impl_->foundation->resource_ring().generation() &&
           impl_->foundation->resource_ring().cancel_capture(snapshot.slot);
  }

  bool
  hdr_analysis_t::submit(
    const writable_snapshot_t &snapshot,
    std::uint64_t source_frame) {
    if (!available() || snapshot.slot >= resource_ring_t::slot_count ||
        snapshot.generation !=
          impl_->foundation->resource_ring().generation()) {
      return false;
    }
    auto &ring = impl_->foundation->resource_ring();
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
    if (SUCCEEDED(status) &&
        !ring.mark_capture_ready(snapshot.slot, capture_ready)) {
      status = E_UNEXPECTED;
    }
    if (SUCCEEDED(status)) {
      status = impl_->record_commands(snapshot.slot);
    }
    if (SUCCEEDED(status)) {
      status = impl_->foundation->compute_queue()->Wait(
        impl_->foundation->shared_fence(),
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
      (void) impl_->foundation->compute_queue()->Wait(
        impl_->foundation->shared_fence(),
        capture_ready);
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
    auto &ring = impl_->foundation->resource_ring();
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
    if (impl_->available && impl_->foundation &&
        impl_->foundation->available()) {
      (void) impl_->foundation->wait_idle();
      (void) poll();
    }
    impl_->available = false;
  }
}  // namespace platf::dxgi::d3d12
