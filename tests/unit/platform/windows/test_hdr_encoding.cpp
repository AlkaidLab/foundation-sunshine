#include <gtest/gtest.h>

#ifdef _WIN32

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

namespace {
  using Microsoft::WRL::ComPtr;

  class shader_includes_t: public ID3DInclude {
  public:
    HRESULT STDMETHODCALLTYPE
    Open(D3D_INCLUDE_TYPE, LPCSTR name, LPCVOID, LPCVOID *data, UINT *size) override {
      std::ifstream file(std::filesystem::path(SUNSHINE_SHADERS_DIR) / name, std::ios::binary);
      if (!file) return E_FAIL;
      const std::string source { std::istreambuf_iterator<char>(file), {} };
      auto copy = new char[source.size()];
      source.copy(copy, source.size());
      *data = copy;
      *size = static_cast<UINT>(source.size());
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE
    Close(LPCVOID data) override {
      delete[] static_cast<const char *>(data);
      return S_OK;
    }
  };

  // Decode the shader result back to display luminance, independently of the
  // production shader implementation. Neutral patches need no gamut conversion.
  double
  decode_nits(double signal, bool hlg, double peak, double gamma) {
    if (hlg) {
      const double scene = signal <= 0.5 ? signal * signal / 3.0 :
                                           (std::exp((signal - 0.55991073) / 0.17883277) + 0.28466892) / 12.0;
      return peak * std::pow(scene, gamma);
    }
    const double p = std::pow(signal, 32.0 / 2523.0);
    return 10000.0 * std::pow(std::max(p - 3424.0 / 4096.0, 0.0) /
                                (2413.0 / 128.0 - 2392.0 / 128.0 * p),
                       16384.0 / 2610.0);
  }

  void
  check_luminance(bool hlg, float peak, float gamma) {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ASSERT_HRESULT_SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
      nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context));

    const std::string source = std::string("#include \"include/") +
                               (hlg ? "convert_hybrid_log_gamma_base.hlsl" : "convert_perceptual_quantizer_base.hlsl") + "\"\n" + R"(
RWStructuredBuffer<float4> output : register(u0);
[numthreads(8, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    static const float nits[8] = {0, 50, 80, 100, 200, 400, 600, 1000};
    output[id.x] = float4(CONVERT_FUNCTION((nits[id.x] / 80.0).xxx), 1.0);
}
)";
    shader_includes_t includes;
    ComPtr<ID3DBlob> bytecode;
    ComPtr<ID3DBlob> errors;
    const auto compiled = D3DCompile(source.data(), source.size(), nullptr, nullptr, &includes,
      "main", "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &bytecode, &errors);
    ASSERT_HRESULT_SUCCEEDED(compiled) << (errors ? static_cast<const char *>(errors->GetBufferPointer()) : "");
    ComPtr<ID3D11ComputeShader> shader;
    ASSERT_HRESULT_SUCCEEDED(device->CreateComputeShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr, &shader));

    D3D11_BUFFER_DESC desc {};
    desc.ByteWidth = 8 * 4 * sizeof(float);
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    desc.StructureByteStride = 4 * sizeof(float);
    ComPtr<ID3D11Buffer> output;
    ASSERT_HRESULT_SUCCEEDED(device->CreateBuffer(&desc, nullptr, &output));
    ComPtr<ID3D11UnorderedAccessView> uav;
    ASSERT_HRESULT_SUCCEEDED(device->CreateUnorderedAccessView(output.Get(), nullptr, &uav));
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    desc.StructureByteStride = 0;
    ComPtr<ID3D11Buffer> readback;
    ASSERT_HRESULT_SUCCEEDED(device->CreateBuffer(&desc, nullptr, &readback));

    // Exercise the old b3 white-gain slots as well as their neutral values.
    // They must no longer alter pixels; HLG must still honor peak and gamma.
    for (const auto legacy : { std::array<float, 2> { 1.0f, 0.0f }, { 2.5f, 2.5f }, { 0.5f, 5.0f } }) {
      const std::array<float, 4> params { peak, gamma, legacy[0], legacy[1] };
      D3D11_BUFFER_DESC cb_desc {};
      cb_desc.ByteWidth = sizeof(params);
      cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
      const D3D11_SUBRESOURCE_DATA initial { params.data(), 0, 0 };
      ComPtr<ID3D11Buffer> cbuffer;
      ASSERT_HRESULT_SUCCEEDED(device->CreateBuffer(&cb_desc, &initial, &cbuffer));
      context->CSSetShader(shader.Get(), nullptr, 0);
      context->CSSetConstantBuffers(3, 1, cbuffer.GetAddressOf());
      context->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
      context->Dispatch(1, 1, 1);
      ID3D11UnorderedAccessView *null_uav = nullptr;
      context->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);
      context->CopyResource(readback.Get(), output.Get());

      D3D11_MAPPED_SUBRESOURCE mapped {};
      ASSERT_HRESULT_SUCCEEDED(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped));
      const auto pixels = static_cast<const float *>(mapped.pData);
      constexpr std::array<double, 8> nits { 0, 50, 80, 100, 200, 400, 600, 1000 };
      for (size_t i = 0; i < nits.size(); ++i) {
        for (size_t channel = 0; channel < 3; ++channel) {
          EXPECT_NEAR(decode_nits(pixels[i * 4 + channel], hlg, peak, gamma), nits[i], 0.1)
            << "patch=" << nits[i] << ", channel=" << channel << ", legacy gain=" << legacy[0];
        }
      }
      context->Unmap(readback.Get(), 0);
    }
  }

  TEST(HdrEncoding, PqPreservesAbsoluteLuminanceWithoutWhiteGain) {
    check_luminance(false, 10000.0f, 1.0f);
  }

  TEST(HdrEncoding, HlgPreservesLuminanceWithReferenceDisplayParameters) {
    check_luminance(true, 1000.0f, 1.2f);
  }

  TEST(HdrEncoding, HlgPreservesLuminanceWithNonReferenceDisplayParameters) {
    check_luminance(true, 4000.0f, 1.48119f);
  }
}  // namespace

#endif
