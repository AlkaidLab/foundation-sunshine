/**
 * @file tests/tools/d3d12_video_probe.cpp
 * @brief Standalone D3D11/D3D12 sharing and resource-ring bootstrap probe.
 */
#include "src/platform/windows/d3d12/d3d12_device.h"
#include "src/platform/windows/d3d12/d3d12_hdr_analysis.h"

#include <chrono>
#include <cmath>
#include <iostream>
#include <iterator>
#include <thread>

#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

namespace {
  using Microsoft::WRL::ComPtr;
  namespace backend = platf::dxgi::video_backend;

  bool
  probe_adapter(IDXGIAdapter1 *adapter, const DXGI_ADAPTER_DESC1 &desc) {
    constexpr D3D_FEATURE_LEVEL feature_levels[] {
      D3D_FEATURE_LEVEL_11_1,
      D3D_FEATURE_LEVEL_11_0,
    };
    ComPtr<ID3D11Device> d3d11_device;
    ComPtr<ID3D11DeviceContext> d3d11_context;
    D3D_FEATURE_LEVEL selected_feature_level {};
    const auto d3d11_status = D3D11CreateDevice(
      adapter,
      D3D_DRIVER_TYPE_UNKNOWN,
      nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT,
      feature_levels,
      static_cast<UINT>(std::size(feature_levels)),
      D3D11_SDK_VERSION,
      &d3d11_device,
      &selected_feature_level,
      &d3d11_context);
    if (FAILED(d3d11_status)) {
      std::cout << "adapter_luid="
                << static_cast<std::uint32_t>(desc.AdapterLuid.HighPart)
                << ':' << desc.AdapterLuid.LowPart
                << " result=failed stage=d3d11_device_create hresult=0x"
                << std::hex << static_cast<std::uint32_t>(d3d11_status)
                << std::dec << '\n';
      return false;
    }

    platf::dxgi::d3d12::device_t d3d12_device;
    const auto result = d3d12_device.initialize(
      adapter,
      d3d11_device.Get(),
      d3d11_context.Get());
    bool hdr_analysis_ready = false;
    std::string_view hdr_analysis_stage = "foundation_unavailable";
    HRESULT hdr_analysis_hresult = S_OK;
    platf::dxgi::d3d12::hdr_final_result_t observed_hdr_result;
    if (result.success) {
      platf::dxgi::d3d12::hdr_analysis_t hdr_analysis;
      const auto hdr_init = hdr_analysis.initialize(
        d3d12_device,
        d3d11_device.Get(),
        d3d11_context.Get(),
        64,
        64,
        64,
        64,
        1);
      hdr_analysis_stage = hdr_init.stage;
      hdr_analysis_hresult = hdr_init.hresult;
      if (hdr_init.success) {
        const auto snapshot = hdr_analysis.try_acquire_snapshot();
        if (snapshot) {
          D3D11_TEXTURE2D_DESC source_desc {};
          source_desc.Width = 64;
          source_desc.Height = 64;
          source_desc.MipLevels = 1;
          source_desc.ArraySize = 1;
          source_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
          source_desc.SampleDesc.Count = 1;
          source_desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
          ComPtr<ID3D11Texture2D> source;
          d3d11_device->CreateTexture2D(
            &source_desc,
            nullptr,
            &source);
          ComPtr<ID3D11UnorderedAccessView> source_uav;
          d3d11_device->CreateUnorderedAccessView(
            source.Get(),
            nullptr,
            &source_uav);
          const FLOAT cell_statistics[4] {
            100.0f,
            400.0f,
            200.0f,
            300.0f,
          };
          d3d11_context->ClearUnorderedAccessViewFloat(
            source_uav.Get(),
            cell_statistics);
          d3d11_context->CopyResource(
            snapshot->texture,
            source.Get());
          if (hdr_analysis.submit(*snapshot, 7)) {
            const auto deadline =
              std::chrono::steady_clock::now() +
              std::chrono::seconds(2);
            while (std::chrono::steady_clock::now() < deadline) {
              if (const auto completed = hdr_analysis.poll()) {
                observed_hdr_result = completed->result;
                const auto summary =
                  platf::dxgi::d3d12::summarize_hdr_result(
                    completed->result);
                hdr_analysis_ready =
                  summary.valid &&
                  completed->source_frame == 7 &&
                  completed->result.pixel_count == 64 * 64 &&
                  std::abs(summary.min_maxrgb - 100.0f) < 0.01f &&
                  std::abs(summary.max_maxrgb - 400.0f) < 0.01f &&
                  std::abs(summary.avg_maxrgb - 200.0f) < 0.01f;
                hdr_analysis_stage =
                  hdr_analysis_ready ? "ready" : "result_mismatch";
                break;
              }
              std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (!hdr_analysis_ready && hdr_analysis_stage == "ready") {
              hdr_analysis_stage = "timeout";
            }
          }
          else {
            hdr_analysis_stage = hdr_analysis.failure_stage();
            hdr_analysis_hresult = hdr_analysis.failure_hresult();
          }
        }
        else {
          hdr_analysis_stage = "snapshot_busy";
        }
      }
    }
    std::cout << "adapter_luid="
              << static_cast<std::uint32_t>(desc.AdapterLuid.HighPart)
              << ':' << desc.AdapterLuid.LowPart
              << " vendor=0x" << std::hex << desc.VendorId
              << " device=0x" << desc.DeviceId
              << std::dec
              << " result=" << (result.success ? "ready" : "failed")
              << " stage=" << result.stage
              << " reason=" << backend::to_string(result.reason)
              << " shared_tier="
              << static_cast<unsigned>(
                   result.capabilities.shared_resource_tier)
              << " shared_fence="
              << (result.capabilities.shared_fence ? "yes" : "no")
              << " topology_a_nv12="
              << (result.capabilities.nv12_encoder_surface ? "yes" : "no")
              << " topology_a_p010="
              << (result.capabilities.p010_encoder_surface ? "yes" : "no")
              << " topology_b_rgba16f="
              << (result.capabilities.rgba16f_bridge ? "yes" : "no")
              << " hdr_analysis="
              << (hdr_analysis_ready ? "ready" : "failed")
              << " hdr_stage=" << hdr_analysis_stage
              << " hdr_hresult=0x" << std::hex
              << static_cast<std::uint32_t>(hdr_analysis_hresult)
              << std::dec
              << " hdr_min=" << observed_hdr_result.min_maxrgb
              << " hdr_max=" << observed_hdr_result.max_maxrgb
              << " hdr_sum=" << observed_hdr_result.sum_maxrgb
              << " hdr_pixels=" << observed_hdr_result.pixel_count
              << " hresult=0x" << std::hex
              << static_cast<std::uint32_t>(result.hresult)
              << std::dec << '\n';
    return result.success && hdr_analysis_ready;
  }
}  // namespace

int
main() {
  ComPtr<IDXGIFactory1> factory;
  const auto factory_status = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
  if (FAILED(factory_status)) {
    std::cerr << "factory_create_failed=0x" << std::hex
              << static_cast<std::uint32_t>(factory_status) << '\n';
    return 1;
  }

  bool any_ready = false;
  for (UINT index = 0;; ++index) {
    ComPtr<IDXGIAdapter1> adapter;
    if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) {
      break;
    }
    DXGI_ADAPTER_DESC1 desc {};
    if (FAILED(adapter->GetDesc1(&desc)) ||
        desc.VendorId == 0x1414 ||
        (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
      continue;
    }
    any_ready |= probe_adapter(adapter.Get(), desc);
  }
  return any_ready ? 0 : 1;
}
