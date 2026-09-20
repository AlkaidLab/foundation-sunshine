// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Foundation Sunshine contributors
#include "nvof_provider.h"
#include "../include/nvof/nvOpticalFlowD3D11.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <vector>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

namespace {
  std::atomic<bool> faulted { false };
#pragma optimize("", off)
  __declspec(noinline) NV_OF_STATUS invoke_raw(NV_OF_STATUS (*body)(void *), void *data) noexcept {
    if (faulted.load()) return NV_OF_ERR_GENERIC;
    __try {
      return body(data);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
      faulted.store(true);
      return NV_OF_ERR_GENERIC;
    }
  }
#pragma optimize("", on)
  template <class F>
  NV_OF_STATUS
  invoke(F &&f) noexcept {
    return invoke_raw([](void *value) { return (*static_cast<F *>(value))(); }, &f);
  }
#define NVOF_CALL(expression) invoke([&]() noexcept { return (expression); })
  constexpr char shaders[] = R"(
Texture2D<float4> color : register(t0);
Texture2D<int2> grid : register(t1);
SamplerState linearClamp : register(s0);
RWTexture2D<float2> dense : register(u0);
struct Vertex { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Vertex vs(uint id : SV_VertexID) {
  Vertex o; o.uv = float2((id << 1) & 2, id & 2);
  o.position = float4(o.uv * float2(2,-2) + float2(-1,1),0,1); return o;
}
float4 ps(Vertex i) : SV_Target { return color.SampleLevel(linearClamp,i.uv,0); }
[numthreads(8,8,1)] void cs(uint3 id : SV_DispatchThreadID) {
  uint w,h,gw,gh; dense.GetDimensions(w,h); grid.GetDimensions(gw,gh);
  if (id.x >= w || id.y >= h) return;
  // Grid samples represent centers of 4x4 cells in the half-resolution image.
  float2 halfSize = floor((float2(w,h)+1)/2);
  float2 at = (float2(id.xy)+0.5)*halfSize/float2(w,h)/4-0.5;
  int2 base = int2(floor(at)); float2 f=frac(at); int2 hi=int2(gw,gh)-1;
  float2 a=grid.Load(int3(clamp(base,int2(0,0),hi),0));
  float2 b=grid.Load(int3(clamp(base+int2(1,0),int2(0,0),hi),0));
  float2 c=grid.Load(int3(clamp(base+int2(0,1),int2(0,0),hi),0));
  float2 d=grid.Load(int3(clamp(base+int2(1,1),int2(0,0),hi),0));
  dense[id.xy]=lerp(lerp(a,b,f.x),lerp(c,d,f.x),f.y)/32*float2(w,h)/halfSize;
}
)";

  bool
  compile(const char *entry, const char *target, ComPtr<ID3DBlob> &blob) {
    ComPtr<ID3DBlob> errors;
    return SUCCEEDED(D3DCompile(shaders, std::strlen(shaders), "flow", nullptr, nullptr,
      entry, target, D3DCOMPILE_ENABLE_STRICTNESS, 0, &blob, &errors));
  }
  struct state_scope {
    ID3D11DeviceContext1 *context;
    ComPtr<ID3DDeviceContextState> previous;
    state_scope(ID3D11DeviceContext1 *c, ID3DDeviceContextState *state):
        context(c) {
      context->SwapDeviceContextState(state, &previous);
    }
    ~state_scope() { context->SwapDeviceContextState(previous.Get(), nullptr); }
  };
}  // namespace

struct flow_provider::impl {
  HMODULE module = nullptr;
  NV_OF_D3D11_API_FUNCTION_LIST api {};
  NvOFHandle session = nullptr;
  NvOFGPUBufferHandle handles[3] {};
  ComPtr<ID3D11Device1> device;
  ComPtr<ID3D11DeviceContext1> context;
  ComPtr<ID3DDeviceContextState> state;
  ComPtr<ID3D11Texture2D> inputs[2], grid;
  ComPtr<ID3D11RenderTargetView> targets[2];
  ComPtr<ID3D11ShaderResourceView> source, grid_view;
  ComPtr<ID3D11UnorderedAccessView> output;
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps;
  ComPtr<ID3D11ComputeShader> cs;
  ComPtr<ID3D11SamplerState> sampler;
  UINT width = 0, height = 0, half_width = 0, half_height = 0, slot = 0;
  bool previous = false, failed = false;
  ~impl() {
    ComPtr<ID3DDeviceContextState> saved;
    if (context && state) context->SwapDeviceContextState(state.Get(), &saved);
    for (auto handle : handles)
      if (handle) NVOF_CALL(api.nvOFUnregisterResourceD3D11(handle));
    if (session) NVOF_CALL(api.nvOFDestroy(session));
    if (context && state) context->SwapDeviceContextState(saved.Get(), nullptr);
    if (module && !faulted.load()) FreeLibrary(module);
  }
  bool
  format(NV_OF_BUFFER_USAGE usage, DXGI_FORMAT wanted) {
    uint32_t count = 0;
    if (NVOF_CALL(api.nvOFGetSurfaceFormatCountD3D11(session, usage, NV_OF_MODE_OPTICALFLOW, &count)) != NV_OF_SUCCESS || !count || count > 64) return false;
    std::vector<DXGI_FORMAT> values(count);
    return NVOF_CALL(api.nvOFGetSurfaceFormatD3D11(session, usage, NV_OF_MODE_OPTICALFLOW, values.data())) == NV_OF_SUCCESS &&
           std::find(values.begin(), values.end(), wanted) != values.end();
  }
  std::vector<uint32_t>
  caps(NV_OF_CAPS capability) {
    uint32_t count = 0;
    if (NVOF_CALL(api.nvOFGetCaps(session, capability, nullptr, &count)) != NV_OF_SUCCESS || !count || count > 64) return {};
    std::vector<uint32_t> values(count);
    if (NVOF_CALL(api.nvOFGetCaps(session, capability, values.data(), &count)) != NV_OF_SUCCESS || count > values.size()) return {};
    values.resize(count);
    return values;
  }
  bool
  supports_dimensions() {
    auto minW = caps(NV_OF_CAPS_WIDTH_MIN), maxW = caps(NV_OF_CAPS_WIDTH_MAX);
    auto minH = caps(NV_OF_CAPS_HEIGHT_MIN), maxH = caps(NV_OF_CAPS_HEIGHT_MAX);
    auto grids = caps(NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES);
    return minW.size() == 1 && maxW.size() == 1 && minH.size() == 1 && maxH.size() == 1 &&
           half_width >= minW[0] && half_width <= maxW[0] && half_height >= minH[0] && half_height <= maxH[0] &&
           std::find(grids.begin(), grids.end(), 4u) != grids.end();
  }
};

flow_provider::flow_provider(std::unique_ptr<impl> value):
    p(std::move(value)) {}
flow_provider::~flow_provider() = default;
std::unique_ptr<flow_provider>
flow_provider::create(ID3D11Device *device, ID3D11DeviceContext *context,
  ID3D11Texture2D *source, ID3D11Texture2D *output, int quality) {
  if (!device || !context || !source || !output || quality < 1 || quality > 3) return {};
  auto p = std::make_unique<impl>();
  if (FAILED(device->QueryInterface(IID_PPV_ARGS(&p->device))) || FAILED(context->QueryInterface(IID_PPV_ARGS(&p->context)))) return {};
  D3D11_TEXTURE2D_DESC desc {}, outdesc {};
  source->GetDesc(&desc);
  output->GetDesc(&outdesc);
  p->width = desc.Width;
  p->height = desc.Height;
  p->half_width = (desc.Width + 1) / 2;
  p->half_height = (desc.Height + 1) / 2;
  if (outdesc.Width != desc.Width || outdesc.Height != desc.Height || outdesc.Format != DXGI_FORMAT_R16G16_FLOAT) return {};
  D3D_FEATURE_LEVEL level = device->GetFeatureLevel();
  if (FAILED(p->device->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), nullptr, &p->state))) return {};
  // Includes driver initialization, which must not leak state into the caller.
  state_scope scope(p->context.Get(), p->state.Get());
  p->module = LoadLibraryExW(L"nvofapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!p->module) return {};
  using Create = NV_OF_STATUS(NVOFAPI *)(uint32_t, NV_OF_D3D11_API_FUNCTION_LIST *);
  auto create = reinterpret_cast<Create>(GetProcAddress(p->module, "NvOFAPICreateInstanceD3D11"));
  if (!create || NVOF_CALL(create(NV_OF_API_VERSION, &p->api)) != NV_OF_SUCCESS) return {};
  if (!p->api.nvCreateOpticalFlowD3D11 || !p->api.nvOFGetSurfaceFormatCountD3D11 ||
      !p->api.nvOFGetSurfaceFormatD3D11 || !p->api.nvOFRegisterResourceD3D11 ||
      !p->api.nvOFUnregisterResourceD3D11 || !p->api.nvOFInit || !p->api.nvOFExecute || !p->api.nvOFDestroy || !p->api.nvOFGetCaps) return {};
  if (NVOF_CALL(p->api.nvCreateOpticalFlowD3D11(device, context, &p->session)) != NV_OF_SUCCESS) return {};
  if (!p->supports_dimensions()) return {};
  if (!p->format(NV_OF_BUFFER_USAGE_INPUT, DXGI_FORMAT_B8G8R8A8_UNORM) || !p->format(NV_OF_BUFFER_USAGE_OUTPUT, DXGI_FORMAT_R16G16_SINT)) return {};
  NV_OF_INIT_PARAMS params {};
  params.width = p->half_width;
  params.height = p->half_height;
  params.outGridSize = NV_OF_OUTPUT_VECTOR_GRID_SIZE_4;
  params.mode = NV_OF_MODE_OPTICALFLOW;
  params.perfLevel = quality == 1 ? NV_OF_PERF_LEVEL_FAST : quality == 2 ? NV_OF_PERF_LEVEL_MEDIUM :
                                                                           NV_OF_PERF_LEVEL_SLOW;
  params.inputBufferFormat = NV_OF_BUFFER_FORMAT_ABGR8;
  if (NVOF_CALL(p->api.nvOFInit(p->session, &params)) != NV_OF_SUCCESS) return {};
  D3D11_TEXTURE2D_DESC half {};
  half.Width = p->half_width;
  half.Height = p->half_height;
  half.MipLevels = 1;
  half.ArraySize = 1;
  half.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  half.SampleDesc.Count = 1;
  half.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  for (int i = 0; i < 2; ++i) {
    if (FAILED(device->CreateTexture2D(&half, nullptr, &p->inputs[i])) ||
        FAILED(device->CreateRenderTargetView(p->inputs[i].Get(), nullptr, &p->targets[i])) ||
        NVOF_CALL(p->api.nvOFRegisterResourceD3D11(p->session, p->inputs[i].Get(), &p->handles[i])) != NV_OF_SUCCESS) return {};
  }
  half.Width = (half.Width + 3) / 4;
  half.Height = (half.Height + 3) / 4;
  half.Format = DXGI_FORMAT_R16G16_SINT;
  half.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  if (FAILED(device->CreateTexture2D(&half, nullptr, &p->grid)) ||
      FAILED(device->CreateShaderResourceView(p->grid.Get(), nullptr, &p->grid_view)) ||
      FAILED(device->CreateShaderResourceView(source, nullptr, &p->source)) ||
      FAILED(device->CreateUnorderedAccessView(output, nullptr, &p->output)) ||
      NVOF_CALL(p->api.nvOFRegisterResourceD3D11(p->session, p->grid.Get(), &p->handles[2])) != NV_OF_SUCCESS) return {};
  ComPtr<ID3DBlob> blob;
  if (!compile("vs", "vs_5_0", blob) || FAILED(device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &p->vs))) return {};
  blob.Reset();
  if (!compile("ps", "ps_5_0", blob) || FAILED(device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &p->ps))) return {};
  blob.Reset();
  if (!compile("cs", "cs_5_0", blob) || FAILED(device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &p->cs))) return {};
  D3D11_SAMPLER_DESC sampling {};
  sampling.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sampling.AddressU = sampling.AddressV = sampling.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampling.MaxLOD = D3D11_FLOAT32_MAX;
  if (FAILED(device->CreateSamplerState(&sampling, &p->sampler))) return {};
  // Restore before moving p: scope keeps a raw pointer to its context.
  return std::unique_ptr<flow_provider>(new flow_provider(std::move(p)));
}

bool
flow_provider::process(bool reset) {
  auto &s = *p;
  state_scope scope(s.context.Get(), s.state.Get());
  // The private state may retain bindings changed by the optical-flow driver.
  s.context->ClearState();
  const float zero[4] {};
  if (s.failed) {
    s.context->ClearUnorderedAccessViewFloat(s.output.Get(), zero);
    return false;
  }
  auto *c = s.context.Get();
  D3D11_VIEWPORT viewport { 0, 0, float(s.half_width), float(s.half_height), 0, 1 };
  c->RSSetViewports(1, &viewport);
  c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  c->VSSetShader(s.vs.Get(), nullptr, 0);
  c->PSSetShader(s.ps.Get(), nullptr, 0);
  auto *target = s.targets[s.slot].Get();
  c->OMSetRenderTargets(1, &target, nullptr);
  auto *view = s.source.Get();
  c->PSSetShaderResources(0, 1, &view);
  auto *sampler = s.sampler.Get();
  c->PSSetSamplers(0, 1, &sampler);
  c->Draw(3, 0);
  c->OMSetRenderTargets(0, nullptr, nullptr);
  view = nullptr;
  c->PSSetShaderResources(0, 1, &view);
  if (!s.previous || reset) {
    c->ClearUnorderedAccessViewFloat(s.output.Get(), zero);
    s.previous = true;
    s.slot ^= 1;
    return true;
  }
  NV_OF_EXECUTE_INPUT_PARAMS in {};
  in.inputFrame = s.handles[s.slot];
  in.referenceFrame = s.handles[s.slot ^ 1];
  in.disableTemporalHints = NV_OF_TRUE;
  NV_OF_EXECUTE_OUTPUT_PARAMS out {};
  out.outputBuffer = s.handles[2];
  if (NVOF_CALL(s.api.nvOFExecute(s.session, &in, &out)) != NV_OF_SUCCESS) {
    s.failed = true;
    c->ClearUnorderedAccessViewFloat(s.output.Get(), zero);
    return false;
  }
  c->CSSetShader(s.cs.Get(), nullptr, 0);
  view = s.grid_view.Get();
  c->CSSetShaderResources(1, 1, &view);
  auto *uav = s.output.Get();
  c->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
  c->Dispatch((s.width + 7) / 8, (s.height + 7) / 8, 1);
  uav = nullptr;
  c->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
  view = nullptr;
  c->CSSetShaderResources(1, 1, &view);
  s.slot ^= 1;
  return true;
}
