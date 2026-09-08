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
#include <numeric>
#include <thread>
#include <vector>

#include <d3d11.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

namespace {
  using Microsoft::WRL::ComPtr;
  namespace backend = platf::dxgi::video_backend;

  // Stall compute while the producer fills every slot. Capture signals must
  // not advance the completion fence or permit an early readback/reuse.
  bool
  probe_ring(platf::dxgi::d3d12::device_t &device,
    platf::dxgi::d3d12::hdr_analysis_t &analysis,
    ID3D11DeviceContext *context) {
    if (FAILED(device.wait_idle())) {
      return false;
    }
    ComPtr<ID3D12Fence> gate;
    if (FAILED(device.device()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)))) {
      return false;
    }
    const auto before = device.shared_fence()->GetCompletedValue();
    if (FAILED(device.compute_queue()->Wait(gate.Get(), 1))) {
      return false;
    }
    const auto submit = [&](std::uint64_t frame) {
      const auto snapshot = analysis.try_acquire_snapshot();
      if (!snapshot) return false;
      const FLOAT base = 100.0f + static_cast<FLOAT>(frame % 17);
      const FLOAT values[4] { base, base + 300.0f, base + 100.0f, base + 200.0f };
      const FLOAT pq[4] { 0.25f + static_cast<FLOAT>(frame % 5) * 0.125f, 0, 0, 0 };
      context->ClearUnorderedAccessViewFloat(snapshot->uav, values);
      context->ClearUnorderedAccessViewFloat(snapshot->pq_uav, pq);
      return analysis.submit(*snapshot, frame);
    };
    bool ready = true;
    for (std::uint64_t frame = 10; frame < 13; ++frame) {
      ready = submit(frame) && ready;
    }
    // The standalone probe has no encoder to submit D3D11 work for us.
    context->Flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ready = !analysis.poll() && ready;
    ready = !analysis.try_acquire_snapshot() && ready;
    ready = device.shared_fence()->GetCompletedValue() == before && ready;
    // Always release the gate, including failure paths, before destroying GPU resources.
    gate->Signal(1);
    if (!ready) return false;

    const auto validate = [](const platf::dxgi::d3d12::completed_hdr_result_t &completed) {
      const auto summary = platf::dxgi::d3d12::summarize_hdr_result(completed.result);
      const float base = 100.0f + static_cast<float>(completed.source_frame % 17);
      const float pq = 0.25f + static_cast<float>(completed.source_frame % 5) * 0.125f;
      return summary.valid && completed.result.pixel_count == 4096 &&
             std::abs(summary.min_maxrgb - base) < 0.01f &&
             std::abs(summary.max_maxrgb - (base + 300.0f)) < 0.01f &&
             std::abs(summary.avg_maxrgb - (base + 100.0f)) < 0.01f &&
             std::abs(summary.avg_maxrgb_pq - pq) < 0.0001f &&
             std::accumulate(completed.result.histogram.begin(), completed.result.histogram.end(), std::uint64_t { 0 }) == 4096;
    };
    std::uint64_t next_frame = 13;
    std::uint64_t newest_frame = 0;
    constexpr std::uint64_t last_frame = 521;  // 512 submissions, reusing all three slots.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
      if (auto result = analysis.poll()) {
        if (!validate(*result) || result->source_frame <= newest_frame) return false;
        newest_frame = result->source_frame;
      }
      if (newest_frame == last_frame) return true;
      while (next_frame <= last_frame && submit(next_frame)) ++next_frame;
      context->Flush();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
  }

  bool
  probe_independent_analyzers(platf::dxgi::d3d12::device_t &device,
    platf::dxgi::d3d12::hdr_analysis_t &first,
    ID3D11Device *d3d11_device, ID3D11DeviceContext *context) {
    platf::dxgi::d3d12::hdr_analysis_t second;
    if (!second.initialize(device, d3d11_device, context, 64, 64, 64, 64, 10000.0f, 2).success) {
      return false;
    }
    for (std::uint64_t frame = 0; frame < 3; ++frame) {
      const auto a = first.try_acquire_snapshot();
      const auto b = second.try_acquire_snapshot();
      if (!a || !b || a->generation != 1 || b->generation != 2) return false;
      const FLOAT a_values[4] { 100, 100, 100, 100 };
      const FLOAT b_values[4] { 1000, 1000, 1000, 1000 };
      const FLOAT a_pq[4] { 0.25f, 0, 0, 0 };
      const FLOAT b_pq[4] { 0.75f, 0, 0, 0 };
      context->ClearUnorderedAccessViewFloat(a->uav, a_values);
      context->ClearUnorderedAccessViewFloat(a->pq_uav, a_pq);
      context->ClearUnorderedAccessViewFloat(b->uav, b_values);
      context->ClearUnorderedAccessViewFloat(b->pq_uav, b_pq);
      if (!first.submit(*a, 1000 + frame) || !second.submit(*b, 2000 + frame)) return false;
    }
    context->Flush();
    std::uint64_t a_frame = 0, b_frame = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
      if (const auto a = first.poll()) {
        const auto stats = platf::dxgi::d3d12::summarize_hdr_result(a->result);
        if (a->generation != 1 || !stats.valid || stats.avg_maxrgb != 100 || stats.avg_maxrgb_pq != 0.25f) return false;
        a_frame = a->source_frame;
      }
      if (const auto b = second.poll()) {
        const auto stats = platf::dxgi::d3d12::summarize_hdr_result(b->result);
        if (b->generation != 2 || !stats.valid || stats.avg_maxrgb != 1000 || stats.avg_maxrgb_pq != 0.75f) return false;
        b_frame = b->source_frame;
      }
      if (a_frame == 1002 && b_frame == 2002) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
  }

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
    ComPtr<ID3D12InfoQueue> debug_queue;
    if (result.success && SUCCEEDED(d3d12_device.device()->QueryInterface(IID_PPV_ARGS(&debug_queue)))) {
      // Capability probes may intentionally attempt unsupported formats.
      debug_queue->ClearStoredMessages();
    }
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
        10000.0f,
        1);
      hdr_analysis_stage = hdr_init.stage;
      hdr_analysis_hresult = hdr_init.hresult;
      if (hdr_init.success) {
        const auto snapshot = hdr_analysis.try_acquire_snapshot();
        if (snapshot) {
          const FLOAT cell_statistics[4] { 100.0f, 400.0f, 200.0f, 300.0f };
          const FLOAT pq_average[4] { 0.5f, 0.0f, 0.0f, 0.0f };
          d3d11_context->ClearUnorderedAccessViewFloat(snapshot->uav, cell_statistics);
          d3d11_context->ClearUnorderedAccessViewFloat(snapshot->pq_uav, pq_average);
          if (hdr_analysis.submit(*snapshot, 7)) {
            d3d11_context->Flush();
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
                  std::abs(summary.avg_maxrgb - 200.0f) < 0.01f &&
                  std::abs(summary.avg_maxrgb_pq - 0.5f) < 0.0001f &&
                  std::accumulate(completed->result.histogram.begin(), completed->result.histogram.end(), std::uint64_t { 0 }) == 64 * 64;
                hdr_analysis_stage =
                  hdr_analysis_ready ? "ready" : "result_mismatch";
                break;
              }
              std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (hdr_analysis_ready && !probe_ring(d3d12_device, hdr_analysis, d3d11_context.Get())) {
              hdr_analysis_ready = false;
              hdr_analysis_stage = "ring_stress_failed";
            }
            if (hdr_analysis_ready && !probe_independent_analyzers(d3d12_device, hdr_analysis, d3d11_device.Get(), d3d11_context.Get())) {
              hdr_analysis_ready = false;
              hdr_analysis_stage = "multi_analyzer_failed";
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
    if (debug_queue) {
      for (UINT64 index = 0; index < debug_queue->GetNumStoredMessages(); ++index) {
        SIZE_T size = 0;
        debug_queue->GetMessage(index, nullptr, &size);
        std::vector<std::byte> storage(size);
        auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
        if (SUCCEEDED(debug_queue->GetMessage(index, message, &size)) &&
            message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
          std::cerr << "d3d12_debug_error=" << message->pDescription << '\n';
          hdr_analysis_ready = false;
          hdr_analysis_stage = "debug_validation_failed";
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
              << " hdr_sum_pq=" << observed_hdr_result.sum_maxrgb_pq
              << " hdr_pixels=" << observed_hdr_result.pixel_count
              << " hresult=0x" << std::hex
              << static_cast<std::uint32_t>(result.hresult)
              << std::dec << '\n';
    return result.success && hdr_analysis_ready;
  }
}  // namespace

int
main(int argc, char **argv) {
  if (argc > 1 && std::string_view(argv[1]) == "--debug") {
    ComPtr<ID3D12Debug> debug;
    if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
      std::cerr << "D3D12 debug layer unavailable\n";
      return 1;
    }
    debug->EnableDebugLayer();
  }
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
