#include <gtest/gtest.h>

#ifdef _WIN32

  #include <algorithm>
  #include <array>
  #include <cmath>
  #include <cstdint>
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

  double
  encode_nits(double nits, bool hlg, double peak, double gamma) {
    if (hlg) {
      const double scene = std::pow(nits / peak, 1.0 / gamma);
      return scene <= 1.0 / 12.0 ? std::sqrt(3.0 * scene) :
                                   0.17883277 * std::log(12.0 * scene - 0.28466892) + 0.55991073;
    }
    const double p = std::pow(nits / 10000.0, 2610.0 / 16384.0);
    return std::pow((3424.0 / 4096.0 + (2413.0 / 128.0) * p) /
                      (1.0 + (2392.0 / 128.0) * p),
      2523.0 / 32.0);
  }

  void
  check_vpe_p010_analysis(bool hlg, bool full_range, float peak, float gamma, bool raw_source = false) {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ASSERT_HRESULT_SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
      nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context));
    ComPtr<ID3DBlob> bytecode, errors;
    const auto path = std::filesystem::path(SUNSHINE_SHADERS_DIR) / "hdr_luminance_analysis_cs.hlsl";
    ASSERT_HRESULT_SUCCEEDED(D3DCompileFromFile(path.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
      "main_cs", "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &bytecode, &errors))
      << (errors ? static_cast<const char *>(errors->GetBufferPointer()) : "");
    ComPtr<ID3D11ComputeShader> shader;
    ASSERT_HRESULT_SUCCEEDED(device->CreateComputeShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr, &shader));

    // Separate R16/R16G16 textures model P010 plane views on WARP. The active
    // scaled picture is 4x2 at offset (2,2); bright padding must not affect stats.
    std::array<uint16_t, 64> y;
    y.fill(static_cast<uint16_t>((full_range ? 1023 : 940) << 6));
    std::array<uint16_t, 32> uv;
    uv.fill(512 << 6);
    std::array<float, 64 * 4> raw_pixels {};
    const std::array<double, 8> patches { 0, 10, 20, 40, 80, 100, 160, 200 };
    double expected_sum = 0, expected_pq_sum = 0, expected_min = 10000, expected_max = 0;
    for (size_t i = 0; i < patches.size(); ++i) {
      const auto code = static_cast<uint16_t>(std::lround(encode_nits(patches[i], hlg, peak, gamma) *
                                                            (full_range ? 1023 : 876) +
                                                          (full_range ? 0 : 64)));
      y[(2 + i / 4) * 8 + 2 + i % 4] = code << 6;
      const double actual_signal = (code - (full_range ? 0.0 : 64.0)) / (full_range ? 1023.0 : 876.0);
      const double actual_nits = raw_source ? patches[i] : decode_nits(actual_signal, hlg, peak, gamma);
      const size_t raw_index = (i / 4 * 8 + i % 4) * 4;
      for (size_t channel = 0; channel < 3; ++channel) raw_pixels[raw_index + channel] = static_cast<float>(patches[i] / 80.0);
      expected_sum += actual_nits;
      expected_pq_sum += encode_nits(actual_nits, false, 10000, 1);
      expected_min = std::min(expected_min, actual_nits);
      expected_max = std::max(expected_max, actual_nits);
    }
    D3D11_TEXTURE2D_DESC texture_desc {};
    texture_desc.Width = texture_desc.Height = 8;
    texture_desc.MipLevels = texture_desc.ArraySize = texture_desc.SampleDesc.Count = 1;
    texture_desc.Format = raw_source ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R16_UNORM;
    texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA y_data { raw_source ? static_cast<const void *>(raw_pixels.data()) : y.data(),
      static_cast<UINT>(raw_source ? 8 * 4 * sizeof(float) : 8 * sizeof(uint16_t)), 0 };
    ComPtr<ID3D11Texture2D> y_texture, uv_texture;
    ASSERT_HRESULT_SUCCEEDED(device->CreateTexture2D(&texture_desc, &y_data, &y_texture));
    texture_desc.Width = texture_desc.Height = 4;
    texture_desc.Format = DXGI_FORMAT_R16G16_UNORM;
    const D3D11_SUBRESOURCE_DATA uv_data { uv.data(), 8 * sizeof(uint16_t), 0 };
    ASSERT_HRESULT_SUCCEEDED(device->CreateTexture2D(&texture_desc, &uv_data, &uv_texture));
    ComPtr<ID3D11ShaderResourceView> y_srv, uv_srv;
    ASSERT_HRESULT_SUCCEEDED(device->CreateShaderResourceView(y_texture.Get(), nullptr, &y_srv));
    ASSERT_HRESULT_SUCCEEDED(device->CreateShaderResourceView(uv_texture.Get(), nullptr, &uv_srv));

    struct GroupResult {
      float minimum, maximum, sum, sum_pq;
      uint32_t count;
    };
    constexpr UINT groups = 6;  // ceil(33/16) * ceil(17/16), much larger than the scaled image.
    D3D11_BUFFER_DESC buffer_desc {};
    buffer_desc.ByteWidth = groups * sizeof(GroupResult);
    buffer_desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    buffer_desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    buffer_desc.StructureByteStride = sizeof(GroupResult);
    ComPtr<ID3D11Buffer> group_results, readback, histogram, histogram_readback;
    ComPtr<ID3D11UnorderedAccessView> group_uav, hist_uav;
    ASSERT_HRESULT_SUCCEEDED(device->CreateBuffer(&buffer_desc, nullptr, &group_results));
    ASSERT_HRESULT_SUCCEEDED(device->CreateUnorderedAccessView(group_results.Get(), nullptr, &group_uav));
    buffer_desc.Usage = D3D11_USAGE_STAGING;
    buffer_desc.BindFlags = buffer_desc.MiscFlags = buffer_desc.StructureByteStride = 0;
    buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ASSERT_HRESULT_SUCCEEDED(device->CreateBuffer(&buffer_desc, nullptr, &readback));
    buffer_desc.ByteWidth = 256 * sizeof(uint32_t);
    ASSERT_HRESULT_SUCCEEDED(device->CreateBuffer(&buffer_desc, nullptr, &histogram_readback));
    buffer_desc.Usage = D3D11_USAGE_DEFAULT;
    buffer_desc.CPUAccessFlags = 0;
    buffer_desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    ASSERT_HRESULT_SUCCEEDED(device->CreateBuffer(&buffer_desc, nullptr, &histogram));
    D3D11_UNORDERED_ACCESS_VIEW_DESC hist_desc {};
    hist_desc.Format = DXGI_FORMAT_R32_UINT;
    hist_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    hist_desc.Buffer.NumElements = 256;
    ASSERT_HRESULT_SUCCEEDED(device->CreateUnorderedAccessView(histogram.Get(), &hist_desc, &hist_uav));
    const UINT zero[4] {};
    context->ClearUnorderedAccessViewUint(hist_uav.Get(), zero);

    struct Parameters {
      uint32_t analysis_width, analysis_height, source_width, source_height, mode;
      float maximum_nits;
      uint32_t offset_x, offset_y;
    } params { 33, 17, 4, 2, raw_source ? 0u : static_cast<uint32_t>((hlg ? 4 : 2) + full_range), hlg ? peak : 10000.0f,
      raw_source ? 0u : 2u, raw_source ? 0u : 2u };
    buffer_desc = {};
    buffer_desc.ByteWidth = sizeof(params);
    buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA initial { &params, 0, 0 };
    ComPtr<ID3D11Buffer> analysis_params, hdr_params;
    ASSERT_HRESULT_SUCCEEDED(device->CreateBuffer(&buffer_desc, &initial, &analysis_params));
    const std::array<float, 4> hdr { peak, gamma, 0, 0 };
    buffer_desc.ByteWidth = sizeof(hdr);
    initial.pSysMem = hdr.data();
    ASSERT_HRESULT_SUCCEEDED(device->CreateBuffer(&buffer_desc, &initial, &hdr_params));
    context->CSSetShader(shader.Get(), nullptr, 0);
    ID3D11ShaderResourceView *srvs[] { y_srv.Get(), nullptr, uv_srv.Get() };
    ID3D11UnorderedAccessView *uavs[] { group_uav.Get(), hist_uav.Get() };
    context->CSSetShaderResources(0, 3, srvs);
    context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
    context->CSSetConstantBuffers(0, 1, analysis_params.GetAddressOf());
    ID3D11Buffer *bound_hdr_params = hlg ? hdr_params.Get() : nullptr;
    context->CSSetConstantBuffers(3, 1, &bound_hdr_params);
    context->Dispatch(3, 2, 1);
    ID3D11UnorderedAccessView *null_uavs[2] {};
    context->CSSetUnorderedAccessViews(0, 2, null_uavs, nullptr);
    context->CopyResource(readback.Get(), group_results.Get());
    context->CopyResource(histogram_readback.Get(), histogram.Get());
    D3D11_MAPPED_SUBRESOURCE mapped {};
    ASSERT_HRESULT_SUCCEEDED(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    const auto values = static_cast<const GroupResult *>(mapped.pData);
    double sum = 0, sum_pq = 0, minimum = 10000, maximum = 0;
    uint32_t count = 0;
    for (UINT i = 0; i < groups; ++i) {
      if (values[i].count == 0) continue;
      sum += values[i].sum;
      sum_pq += values[i].sum_pq;
      count += values[i].count;
      minimum = std::min(minimum, static_cast<double>(values[i].minimum));
      maximum = std::max(maximum, static_cast<double>(values[i].maximum));
    }
    context->Unmap(readback.Get(), 0);
    EXPECT_EQ(count, patches.size());
    EXPECT_NEAR(minimum, expected_min, 0.02);
    EXPECT_NEAR(maximum, expected_max, 0.1);
    EXPECT_NEAR(sum, expected_sum, 0.2);
    EXPECT_NEAR(sum_pq, expected_pq_sum, 0.001);
    ASSERT_HRESULT_SUCCEEDED(context->Map(histogram_readback.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    const auto bins = static_cast<const uint32_t *>(mapped.pData);
    uint32_t histogram_count = 0;
    for (UINT i = 0; i < 256; ++i) histogram_count += bins[i];
    context->Unmap(histogram_readback.Get(), 0);
    EXPECT_EQ(histogram_count, patches.size());
  }

  TEST(HdrEncoding, VpePqAnalysisUsesCroppedScaledOutputForBothRanges) {
    for (const bool full : { false, true }) check_vpe_p010_analysis(false, full, 10000, 1);
  }

  TEST(HdrEncoding, VpeHlgAnalysisHonorsDisplayPeakForBothRanges) {
    for (const bool full : { false, true }) {
      check_vpe_p010_analysis(true, full, 400, 1.032865f);
      check_vpe_p010_analysis(true, full, 1000, 1.2f);
      check_vpe_p010_analysis(true, full, 4000, 1.48119f);
    }
  }

  TEST(HdrEncoding, ShaderFallbackStillAnalyzesLinearScRgb) {
    check_vpe_p010_analysis(false, false, 10000, 1, true);
  }
}  // namespace

#endif
