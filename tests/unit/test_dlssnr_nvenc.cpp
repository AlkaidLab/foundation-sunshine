/** SPDX-License-Identifier: GPL-3.0-only
 * Opt-in synthetic NR -> P010 -> NVENC synchronization diagnostic.
 * This exercises GPU submission, not capture, networking or HDR colour quality.
 */
#include "../tests_common.h"

#ifdef _WIN32
  #include "src/nvenc/win/nvenc_dynamic_factory.h"
  #include "src/platform/windows/pre_encode_filter.h"
  #include <cstdlib>
  #include <cstring>
  #include <d3dcompiler.h>
  #include <wrl/client.h>

TEST(DlssNrHardware, NativeHdrFirstEncodedPacket) {
  const auto adapter = std::getenv("SUNSHINE_TEST_DLSSNR_ADAPTER");
  const auto digest = std::getenv("SUNSHINE_TEST_DLSSNR_SHA256");
  if (!adapter || !digest) {
    GTEST_SKIP() << "Explicit verified NR runtime required";
  }
  using Microsoft::WRL::ComPtr;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ASSERT_HRESULT_SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE,
    nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
    &device, nullptr, &context));
  auto factory = nvenc::nvenc_dynamic_factory::get();
  ASSERT_TRUE(factory);
  auto encoder = factory->create_nvenc_d3d11_native(device.Get());
  ASSERT_TRUE(encoder);
  video::config_t config { .width = 1920, .height = 1080, .framerate = 60, .bitrate = 20000 };
  config.videoFormat = 1;
  config.dynamicRange = 1;
  ASSERT_TRUE(encoder->create_encoder({}, config,
    { video::colorspace_e::bt2020, false, 10 }, platf::pix_fmt_e::p010, true));
  auto filter = platf::dxgi::make_pre_encode_filter(
    platf::pre_encode_filter_e::external_neural_enhancement, device.Get(), context.Get(),
    std::filesystem::path(reinterpret_cast<const char8_t *>(adapter)), {},
    "alkaidlab.nvidia_dlssnr", digest);
  ASSERT_TRUE(filter);
  ASSERT_FALSE(filter->degraded()) << filter->failure_reason();

  D3D11_TEXTURE2D_DESC desc {};
  desc.Width = 1920;
  desc.Height = 1080;
  desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
  desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  std::vector<uint16_t> pixels(1920 * 1080 * 4, 0x3c00);
  D3D11_SUBRESOURCE_DATA data { pixels.data(), 1920 * 8, 0 };
  ComPtr<ID3D11Texture2D> input;
  ComPtr<ID3D11ShaderResourceView> input_srv;
  ASSERT_HRESULT_SUCCEEDED(device->CreateTexture2D(&desc, &data, &input));
  ASSERT_HRESULT_SUCCEEDED(device->CreateShaderResourceView(input.Get(), nullptr, &input_srv));
  // Deliberately minimal conversion: create a GPU dependency on NR output and
  // write legal P010 codes. Production colour conversion is outside this test.
  constexpr char shader[] = R"(
Texture2D<float4> source : register(t0);
RWTexture2D<unorm float> y : register(u0);
RWTexture2D<unorm float2> uv : register(u1);
[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID) {
  if (p.x>=1920 || p.y>=1080) return;
  y[p.xy] = (64 + round(saturate(source[p.xy].r / 4) * 876)) * 64 / 65535.0;
  if ((p.x%2)==0 && (p.y%2)==0) uv[p.xy/2] = float2(32768,32768)/65535.0;
})";
  ComPtr<ID3DBlob> blob, errors;
  ASSERT_HRESULT_SUCCEEDED(D3DCompile(shader, std::strlen(shader), nullptr, nullptr,
    nullptr, "main", "cs_5_0", 0, 0, &blob, &errors));
  ComPtr<ID3D11ComputeShader> convert;
  ASSERT_HRESULT_SUCCEEDED(device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &convert));
  ComPtr<ID3D11UnorderedAccessView> y, uv;
  D3D11_UNORDERED_ACCESS_VIEW_DESC view_desc {};
  view_desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
  view_desc.Format = DXGI_FORMAT_R16_UNORM;
  ASSERT_HRESULT_SUCCEEDED(device->CreateUnorderedAccessView(encoder->get_input_texture(), &view_desc, &y));
  view_desc.Format = DXGI_FORMAT_R16G16_UNORM;
  ASSERT_HRESULT_SUCCEEDED(device->CreateUnorderedAccessView(encoder->get_input_texture(), &view_desc, &uv));
  for (int frame = 0; frame < 3; ++frame) {
    const auto result = filter->process({ .texture = input.Get(), .srv = input_srv.Get(), .format = desc.Format, .semantic = { .domain = platf::frame_domain_e::linear_scrgb, .encoding = platf::pixel_encoding_class_e::float16, .reference_white_nits = 80.0f }, .width = 1920, .height = 1080 });
    ASSERT_EQ(result.status, platf::dxgi::filter_status_e::ready);
    ASSERT_FALSE(filter->degraded()) << filter->failure_reason();
    context->CSSetShaderResources(0, 1, &result.frame.srv);
    ID3D11UnorderedAccessView *views[] { y.Get(), uv.Get() };
    context->CSSetUnorderedAccessViews(0, 2, views, nullptr);
    context->CSSetShader(convert.Get(), nullptr, 0);
    context->Dispatch(240, 135, 1);
    context->ClearState();
    // No CPU readback, caller Flush, or next NR frame can hide a first-packet
    // submission dependency. NVENC must make progress with this queued input.
    const auto packet = encoder->encode_frame(frame, frame == 0);
    ASSERT_FALSE(packet.data.empty());
    if (frame == 0) {
      EXPECT_TRUE(packet.idr);
    }
  }
}
#endif
