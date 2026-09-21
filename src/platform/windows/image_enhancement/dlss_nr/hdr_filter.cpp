/** SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 Foundation Sunshine contributors.
 * Preserve captured HDR while applying an SDR model's bounded enhancement.
 */
#include "hdr_filter.h"
#include "../../pre_encode_filter_helpers.h"

#include <cstring>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

namespace platf::dxgi::image_enhancement::dlss_nr {
  namespace {
    using Microsoft::WRL::ComPtr;
    // scRGB's 1.0 is 80 nits. Normalize the model proxy around 203-nit white.
    // The exact quantized proxy is also sampled by the resolve pass: an
    // unchanged model output produces zero residual even after BGRA8 rounding.
    constexpr char shader[] = R"(
Texture2D<float4> original : register(t0);
Texture2D<float4> proxy : register(t1);
Texture2D<float4> enhanced : register(t2);
SamplerState linear_clamp : register(s0);
cbuffer Settings : register(b0) { uint source_width, source_height, model_width, model_height; uint is_hdr; uint3 padding; };
float3 decode(float3 c) {
  return lerp(c / 12.92, pow(max((c + 0.055) / 1.055, 0), 2.4), step(0.04045, c));
}
float3 encode(float3 c) {
  return lerp(c * 12.92, 1.055 * pow(max(c, 0), 1.0 / 2.4) - 0.055, step(0.0031308, c));
}
float proxy_scale(float3 c) { return 2.5375 + max(0, max(c.r, max(c.g, c.b))); }
float4 vertex(uint id : SV_VertexID) : SV_Position {
  return float4(id == 2 ? 3 : -1, id == 1 ? 3 : -1, 0, 1);
}
float4 prepare(float4 position : SV_Position) : SV_Target {
  float2 uv = position.xy / float2(model_width, model_height);
  float3 c;
  if (source_width == model_width && source_height == model_height) {
    c = original.Load(int3(uint2(position.xy), 0)).rgb;
  } else {
    // Four bilinear taps cover the low-resolution pixel footprint (50–100%).
    float2 d = 0.25 / float2(model_width, model_height);
    c = (original.SampleLevel(linear_clamp, uv + float2(-d.x, -d.y), 0).rgb
       + original.SampleLevel(linear_clamp, uv + float2(d.x, -d.y), 0).rgb
       + original.SampleLevel(linear_clamp, uv + float2(-d.x, d.y), 0).rgb
       + original.SampleLevel(linear_clamp, uv + d, 0).rgb) * 0.25;
  }
  return float4(is_hdr ? saturate(encode(max(c, 0) / proxy_scale(c))) : c, 1);
}
float4 resolve(float4 position : SV_Position) : SV_Target {
  float4 source = original.Load(int3(uint2(position.xy), 0));
  float2 uv = position.xy / float2(source_width, source_height);
  float3 before = proxy.SampleLevel(linear_clamp, uv, 0).rgb;
  float3 after = enhanced.SampleLevel(linear_clamp, uv, 0).rgb;
  // Matched samples make an identity model a zero residual, preserving native
  // text, signed HDR values and highlights instead of enlarging the whole image.
  float3 delta = decode(after) - decode(before);
  float3 result = is_hdr ? clamp(source.rgb + clamp(delta, -0.25, 0.25) * proxy_scale(source.rgb), -65504, 65504)
                        : saturate(encode(max(decode(source.rgb) + delta, 0)));
  return float4(all(isfinite(result)) ? result : source.rgb, source.a);
}
)";

    class hdr_filter_t final: public pre_encode_filter_t {
    public:
      hdr_filter_t(ID3D11Device *device, ID3D11DeviceContext *context,
        std::unique_ptr<pre_encode_filter_t> model, int scale_percent):
          device_(device),
          context_(context), model_(std::move(model)), scale_percent_(scale_percent) {}

      std::string_view
      backend_name() const override { return model_->backend_name(); }
      bool
      degraded() const override { return model_->degraded(); }
      std::string_view
      failure_reason() const override { return model_->failure_reason(); }
      void
      flush() override { model_->flush(); }

      filter_result_t
      process(const gpu_frame_view_t &input) override {
        const bool hdr = input.semantic.domain == frame_domain_e::linear_scrgb;
        if (seen_input_ && hdr != previous_hdr_) model_->flush();
        previous_hdr_ = hdr;
        seen_input_ = true;
        if (!hdr && scale_percent_ == 100) return model_->process(input);
        if (const auto reason = filter_detail::validate_neural_input(input); !reason.empty()) {
          return { .status = filter_status_e::failed, .reason = reason };
        }
        if (!initialize(input.width, input.height, hdr)) {
          return { .status = filter_status_e::failed, .reason = "nr_proxy_initialization_failed" };
        }
        struct state_guard_t {
          ID3D11DeviceContext1 *context;
          ComPtr<ID3DDeviceContextState> previous;
          state_guard_t(ID3D11DeviceContext1 *c, ID3DDeviceContextState *state):
              context(c) {
            context->SwapDeviceContextState(state, &previous);
            context->ClearState();
          }
          ~state_guard_t() {
            context->ClearState();
            context->SwapDeviceContextState(previous.Get(), nullptr);
          }
        } state(context1_.Get(), state_.Get());

        D3D11_VIEWPORT viewport { 0, 0, static_cast<float>(model_width_), static_cast<float>(model_height_), 0, 1 };
        context_->RSSetViewports(1, &viewport);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vertex_.Get(), nullptr, 0);
        const UINT settings[] { input.width, input.height, model_width_, model_height_, hdr ? 1u : 0u, 0, 0, 0 };
        context_->UpdateSubresource(constants_.Get(), 0, nullptr, settings, 0, 0);
        auto constants = constants_.Get();
        auto sampler = sampler_.Get();
        context_->PSSetConstantBuffers(0, 1, &constants);
        context_->PSSetSamplers(0, 1, &sampler);
        context_->PSSetShader(prepare_.Get(), nullptr, 0);
        context_->PSSetShaderResources(0, 1, &input.srv);
        auto rtv = proxy_rtv_.Get();
        context_->OMSetRenderTargets(1, &rtv, nullptr);
        context_->Draw(3, 0);
        context_->ClearState();

        auto proxy_frame = filter_detail::make_sdr_result(input, proxy_.Get(), proxy_srv_.Get()).frame;
        proxy_frame.semantic.reference_white_nits = 0;
        proxy_frame.width = model_width_;
        proxy_frame.height = model_height_;
        auto processed = model_->process(proxy_frame);
        if (processed.status != filter_status_e::ready || !processed.frame.srv) return processed;
        context_->ClearState();
        ID3D11ShaderResourceView *views[] { input.srv, proxy_srv_.Get(), processed.frame.srv };
        viewport.Width = static_cast<float>(input.width);
        viewport.Height = static_cast<float>(input.height);
        context_->RSSetViewports(1, &viewport);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vertex_.Get(), nullptr, 0);
        context_->PSSetShaderResources(0, 3, views);
        context_->PSSetConstantBuffers(0, 1, &constants);
        context_->PSSetSamplers(0, 1, &sampler);
        rtv = output_rtv_.Get();
        context_->OMSetRenderTargets(1, &rtv, nullptr);
        context_->PSSetShader(resolve_.Get(), nullptr, 0);
        context_->Draw(3, 0);
        auto result = input;
        result.format = hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;
        result.texture = output_.Get();
        result.srv = output_srv_.Get();
        return { .status = filter_status_e::ready, .frame = result };
      }

    private:
      bool
      initialize(UINT width, UINT height, bool hdr) {
        if (!state_) {
          ComPtr<ID3D11Device1> device1;
          if (FAILED(device_.As(&device1)) || FAILED(context_.As(&context1_))) return false;
          D3D_FEATURE_LEVEL levels[] { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
          if (FAILED(device1->CreateDeviceContextState(0, levels, 2, D3D11_SDK_VERSION,
                __uuidof(ID3D11Device), nullptr, &state_))) return false;
        }
        if (!vertex_ || !prepare_ || !resolve_) {
          ComPtr<ID3DBlob> v, p, c, errors;
          if (FAILED(D3DCompile(shader, std::strlen(shader), nullptr, nullptr, nullptr, "vertex", "vs_5_0", 0, 0, &v, &errors)) ||
              FAILED(D3DCompile(shader, std::strlen(shader), nullptr, nullptr, nullptr, "prepare", "ps_5_0", 0, 0, &p, &errors)) ||
              FAILED(D3DCompile(shader, std::strlen(shader), nullptr, nullptr, nullptr, "resolve", "ps_5_0", 0, 0, &c, &errors))) return false;
          if (FAILED(device_->CreateVertexShader(v->GetBufferPointer(), v->GetBufferSize(), nullptr, &vertex_)) ||
              FAILED(device_->CreatePixelShader(p->GetBufferPointer(), p->GetBufferSize(), nullptr, &prepare_)) ||
              FAILED(device_->CreatePixelShader(c->GetBufferPointer(), c->GetBufferSize(), nullptr, &resolve_))) return false;
        }
        if (!sampler_) {
          D3D11_SAMPLER_DESC desc {};
          desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
          desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
          desc.MaxLOD = D3D11_FLOAT32_MAX;
          if (FAILED(device_->CreateSamplerState(&desc, &sampler_))) return false;
        }
        if (!constants_) {
          D3D11_BUFFER_DESC desc {};
          desc.ByteWidth = 32;
          desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
          if (FAILED(device_->CreateBuffer(&desc, nullptr, &constants_))) return false;
        }
        if (output_rtv_ && output_srv_ && proxy_srv_ && proxy_rtv_ && width_ == width && height_ == height && output_hdr_ == hdr) return true;
        proxy_.Reset();
        proxy_srv_.Reset();
        proxy_rtv_.Reset();
        output_.Reset();
        output_srv_.Reset();
        output_rtv_.Reset();
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = model_width_ = nr_scaled_dimension(width, scale_percent_);
        desc.Height = model_height_ = nr_scaled_dimension(height, scale_percent_);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, &proxy_)) ||
            FAILED(device_->CreateShaderResourceView(proxy_.Get(), nullptr, &proxy_srv_)) ||
            FAILED(device_->CreateRenderTargetView(proxy_.Get(), nullptr, &proxy_rtv_))) return false;
        desc.Width = width;
        desc.Height = height;
        desc.Format = hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, &output_)) ||
            FAILED(device_->CreateShaderResourceView(output_.Get(), nullptr, &output_srv_)) ||
            FAILED(device_->CreateRenderTargetView(output_.Get(), nullptr, &output_rtv_))) return false;
        output_hdr_ = hdr;
        width_ = width;
        height_ = height;
        return true;
      }

      ComPtr<ID3D11Device> device_;
      ComPtr<ID3D11DeviceContext> context_;
      ComPtr<ID3D11DeviceContext1> context1_;
      ComPtr<ID3DDeviceContextState> state_;
      ComPtr<ID3D11VertexShader> vertex_;
      ComPtr<ID3D11PixelShader> prepare_;
      ComPtr<ID3D11PixelShader> resolve_;
      ComPtr<ID3D11SamplerState> sampler_;
      ComPtr<ID3D11Buffer> constants_;
      ComPtr<ID3D11Texture2D> proxy_, output_;
      ComPtr<ID3D11ShaderResourceView> proxy_srv_, output_srv_;
      ComPtr<ID3D11RenderTargetView> proxy_rtv_;
      ComPtr<ID3D11RenderTargetView> output_rtv_;
      std::unique_ptr<pre_encode_filter_t> model_;
      UINT width_ = 0, height_ = 0, model_width_ = 0, model_height_ = 0;
      int scale_percent_ = 100;
      bool output_hdr_ = false;
      bool seen_input_ = false, previous_hdr_ = false;
    };
  }  // namespace

  std::unique_ptr<pre_encode_filter_t>
  make_hdr_compatible_filter(ID3D11Device *device, ID3D11DeviceContext *context,
    std::unique_ptr<pre_encode_filter_t> model, int scale_percent) {
    if (!device || !context || !model || !valid_nr_scale(scale_percent)) return {};
    return std::make_unique<hdr_filter_t>(device, context, std::move(model), scale_percent);
  }
}  // namespace platf::dxgi::image_enhancement::dlss_nr
