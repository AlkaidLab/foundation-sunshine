/**
 * @file src/platform/windows/d3d12/d3d12_hdr_analysis_pipeline.cpp
 * @brief D3D12 HDR shader pipelines and two-pass command recording.
 */
#include "d3d12_hdr_analysis_internal.h"

#include "generated/windows/d3d12/d3d12_hdr_shaders.h"
#include <algorithm>
#include <d3dcompiler.h>

namespace platf::dxgi::d3d12 {
  using namespace hdr_analysis_detail;

  bool
  hdr_analysis_t::built() {
    return shaders::hdr_luminance_analysis_cs_dxil_size != 0 &&
           shaders::hdr_luminance_reduce_cs_dxil_size != 0;
  }

  HRESULT
  hdr_analysis_t::impl_t::create_pipeline() {
    // embed_dxil.cmake emits zero-sized blobs when DXC was unavailable at
    // configure time. Report that as an ordinary capability miss so the
    // caller keeps the D3D11 analysis path instead of surfacing a driver
    // error the user can do nothing about.
    if (shaders::hdr_luminance_analysis_cs_dxil_size == 0 ||
        shaders::hdr_luminance_reduce_cs_dxil_size == 0) {
      return fail(E_NOTIMPL, "hdr_shader_unavailable");
    }

    D3D12_DESCRIPTOR_RANGE ranges[2] {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 2;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart =
      D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 2;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart =
      D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER parameters[4] {};
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
    parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[3].Descriptor.ShaderRegister = 3;
    parameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC signature_desc {};
    signature_desc.NumParameters = 4;
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
  hdr_analysis_t::impl_t::record_commands(std::size_t slot_index) {
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
    const auto query_base = static_cast<UINT>(slot_index * timing_query_count);
    auto timestamp = [&](UINT offset) {
      if (slot.measured) list->EndQuery(timing_queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query_base + offset);
    };
    timestamp(0);
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
    auto pq_to_srv = snapshot_to_srv;
    pq_to_srv.Transition.pResource = slot.pq_snapshot.Get();
    list->ResourceBarrier(1, &pq_to_srv);

    const UINT clear_values[4] {};
    list->ClearUnorderedAccessViewUint(
      gpu_handle(descriptor_heap.Get(), descriptor_increment, base + 3),
      cpu_handle(clear_descriptor_heap.Get(), descriptor_increment, slot_index),
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
    list->SetComputeRootConstantBufferView(
      3, slot.constants->GetGPUVirtualAddress() + constant_buffer_alignment * 2);
    list->SetComputeRootDescriptorTable(
      1,
      gpu_handle(descriptor_heap.Get(), descriptor_increment, base));
    list->SetComputeRootDescriptorTable(
      2,
      gpu_handle(descriptor_heap.Get(), descriptor_increment, base + 2));
    list->Dispatch(
      (analysis_width + 15) / 16,
      (analysis_height + 15) / 16,
      1);

    D3D12_RESOURCE_BARRIER pass1_barriers[2] {};
    timestamp(1);
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
      gpu_handle(descriptor_heap.Get(), descriptor_increment, base + 4));
    list->SetComputeRootDescriptorTable(
      2,
      gpu_handle(descriptor_heap.Get(), descriptor_increment, base + 6));
    list->Dispatch(1, 1, 1);

    D3D12_RESOURCE_BARRIER final_barrier {};
    timestamp(2);
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

    D3D12_RESOURCE_BARRIER restore[4] {};
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
    restore[3] = pq_to_srv;
    std::swap(restore[3].Transition.StateBefore, restore[3].Transition.StateAfter);
    list->ResourceBarrier(4, restore);
    timestamp(3);
    if (slot.measured) {
      list->ResolveQueryData(timing_queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query_base,
        timing_query_count, slot.readback.Get(), timing_readback_offset);
    }
    status = list->Close();
    return FAILED(status) ?
             fail(status, "hdr_command_list_close") :
             S_OK;
  }
}  // namespace platf::dxgi::d3d12
