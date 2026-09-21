/** SPDX-License-Identifier: GPL-3.0-only
 * Opt-in synthetic NR -> P010 -> NVENC synchronization diagnostic.
 * This exercises GPU submission, not capture, networking or HDR colour quality.
 */
#include "../tests_common.h"

#ifdef _WIN32
  #include "src/image_enhancement/config.h"
  #include "src/nvenc/win/nvenc_dynamic_factory.h"
  #include "src/platform/windows/display.h"
  #include "src/platform/windows/display_vram_internal.h"
  #include "src/platform/windows/pre_encode_filter.h"
  #include <boost/make_shared.hpp>
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

namespace platf::dxgi {
  int
  init();  // Compile the same conversion shaders used by the host.
}
namespace {
  class SyntheticHdrDisplay: public platf::dxgi::display_vram_t {
  public:
    platf::capture_e
    snapshot(const pull_free_image_cb_t &, std::shared_ptr<platf::img_t> &,
      std::chrono::milliseconds, bool) override { return platf::capture_e::timeout; }
    platf::capture_e
    release_snapshot() override { return platf::capture_e::ok; }
    bool
    is_hdr() override { return hdr_capture; }
    bool hdr_capture = true;
    bool
    get_hdr_metadata(SS_HDR_METADATA &metadata) override {
      metadata = {};
      metadata.maxDisplayLuminance = 1000;
      return true;
    }
  };
}  // namespace

static void
exercise_production_conversion(int dynamic_range, bool unavailable_backend = false, bool hdr_capture = true, bool live_toggle = false) {
  const auto adapter_path = std::getenv("SUNSHINE_TEST_DLSSNR_ADAPTER");
  const auto digest = std::getenv("SUNSHINE_TEST_DLSSNR_SHA256");
  if (!adapter_path || !digest) {
    GTEST_SKIP() << "Explicit verified NR runtime required";
  }
  ASSERT_EQ(platf::dxgi::init(), 0);
  auto display = std::make_shared<SyntheticHdrDisplay>();
  display->hdr_capture = hdr_capture;
  display->width = display->width_before_rotation = display->env_width = 3840;
  display->height = display->height_before_rotation = display->env_height = 2160;
  display->offset_x = display->offset_y = 0;
  display->capture_format = hdr_capture ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;
  display->capture_linear_gamma = hdr_capture;
  ASSERT_HRESULT_SUCCEEDED(CreateDXGIFactory1(IID_IDXGIFactory1, reinterpret_cast<void **>(&display->factory)));
  ASSERT_HRESULT_SUCCEEDED(display->factory->EnumAdapters1(0, &display->adapter));
  ASSERT_HRESULT_SUCCEEDED(D3D11CreateDevice(display->adapter.get(), D3D_DRIVER_TYPE_UNKNOWN,
    nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &display->device, nullptr, &display->device_ctx));

  video::config_t config { .width = 1920, .height = 1080, .framerate = 60, .bitrate = 20000 };
  config.videoFormat = 1;
  config.dynamicRange = dynamic_range;
  config.pre_encode_filter = live_toggle ? platf::pre_encode_filter_e::none : platf::pre_encode_filter_e::external_neural_enhancement;
  config.frame_pipeline_policy = platf::resolve_frame_pipeline_policy(dynamic_range, false, true);
  config.frame_pipeline_policy_resolved = true;
  auto backend = boost::make_shared<image_enhancement::backend_use_t>();
  backend->id = "alkaidlab.nvidia_dlssnr";
  backend->path = std::filesystem::path(reinterpret_cast<const char8_t *>(adapter_path));
  backend->runtime_digest = digest;
  if (unavailable_backend) {
    // Reject a runtime pin without modifying the installed files or weakening
    // loader trust. Encoder initialization and subsequent packets must survive.
    backend->runtime_digest = std::string(64, '0');
  }
  config.enhancement_backend = backend;
  display->capture_contract = config.frame_pipeline_policy.capture;
  config.encoderCscMode = 2;  // Rec.709 for the SDR fallback.
  const auto colorspace = video::colorspace_from_client_config(config, display->is_hdr());
  const bool hdr_output = video::colorspace_is_hdr(colorspace);
  ASSERT_EQ(hdr_output, hdr_capture && dynamic_range != 0);
  ASSERT_EQ(colorspace.bit_depth, dynamic_range != 0 ? 10u : 8u);
  auto encoder = display->make_nvenc_encode_device(colorspace.bit_depth == 10 ? platf::pix_fmt_e::p010 : platf::pix_fmt_e::nv12, config);
  ASSERT_TRUE(encoder);
  ASSERT_TRUE(encoder->init_encoder(config, colorspace));
  auto frame = display->alloc_img();
  ASSERT_TRUE(frame);
  ASSERT_EQ(display->complete_img(frame.get(), true), 0);
  auto &image = static_cast<platf::dxgi::img_d3d_t &>(*frame);
  // DDX marks a cursor-only startup placeholder nonblank but leaves its frame
  // semantics unknown. It must encode without entering the HDR NR contract.
  image.blank = false;
  ASSERT_EQ(encoder->convert(image), 0);
  ASSERT_FALSE(encoder->nvenc->encode_frame(0, true).data.empty());
  ASSERT_EQ(display->complete_img(frame.get(), false), 0);
  for (int i = 0; i < 3; ++i) {
    if (live_toggle) {
      const auto state = video::get_hdr_pipeline_statuses();
      ASSERT_EQ(state.size(), 1u);
      ASSERT_TRUE(state[0].nr_toggle_supported);
      ASSERT_EQ(video::request_nr_enabled(state[0].id, i != 1), 202);
    }
    ASSERT_EQ(image.capture_mutex->AcquireSync(0, 5000), S_OK);
    const float colour[] { 0.25f, 1.0f, 4.0f, 1.0f };
    display->device_ctx->ClearRenderTargetView(image.capture_rt.get(), colour);
    ASSERT_HRESULT_SUCCEEDED(image.capture_mutex->ReleaseSync(0));
    ASSERT_EQ(encoder->convert(image), 0);
    const auto packet = encoder->nvenc->encode_frame(i + 1, i == 0);
    ASSERT_FALSE(packet.data.empty());
    if (i == 0) { EXPECT_TRUE(packet.idr); }
    if (live_toggle) {
      const auto state = video::get_hdr_pipeline_statuses();
      ASSERT_EQ(state.size(), 1u);
      EXPECT_EQ(state[0].nr_state, i == 1 ? "disabled" : "active");
      EXPECT_EQ(state[0].nr_requested_enabled, i != 1);
    }
  }
  const auto statuses = video::get_hdr_pipeline_statuses();
  ASSERT_EQ(statuses.size(), 1u);
  EXPECT_EQ(statuses[0].nr_state, unavailable_backend ? "degraded" : "active");
  if (unavailable_backend) {
    EXPECT_EQ(statuses[0].nr_failure_reason, "runtime_untrusted");
  }
  EXPECT_EQ(statuses[0].hdr_mode, !hdr_output ? "sdr" : dynamic_range == 2 ? "hlg" : "pq");
}

TEST(DlssNrHardware, LiveNrTogglePreservesNativeHdrPackets) {
  exercise_production_conversion(1, false, true, true);
}

TEST(DlssNrHardware, LiveNrTogglePreservesSdrPackets) {
  exercise_production_conversion(0, false, false, true);
}

TEST(DlssNrHardware, ProductionSdrCaptureReportsSdrDespiteHdrRequest) {
  exercise_production_conversion(1, false, false);
}

TEST(DlssNrHardware, ProductionHdrCaptureToSdrFirstEncodedPacket) {
  exercise_production_conversion(0);
}

TEST(DlssNrHardware, ProductionUnavailableNrStillEncodesHdrCaptureToSdr) {
  exercise_production_conversion(0, true);
}

TEST(DlssNrHardware, ProductionConversionFirstEncodedPacket) {
  exercise_production_conversion(1);
}

TEST(DlssNrHardware, ProductionHlgConversionFirstEncodedPacket) {
  exercise_production_conversion(2);
}

TEST(DlssNrHardware, ProductionUnavailableNrStillEncodesHdr) {
  exercise_production_conversion(1, true);
}

TEST(DlssNrHardware, DesktopCaptureFirstEncodedPacket) {
  const auto adapter_path = std::getenv("SUNSHINE_TEST_DLSSNR_ADAPTER");
  const auto digest = std::getenv("SUNSHINE_TEST_DLSSNR_SHA256");
  const auto capture = std::getenv("SUNSHINE_TEST_DLSSNR_CAPTURE");
  if (!adapter_path || !digest || !capture || std::strcmp(capture, "1") != 0) {
    GTEST_SKIP() << "Explicit desktop capture opt-in and verified NR runtime required";
  }
  ASSERT_EQ(platf::dxgi::init(), 0);
  video::config_t config { .width = 1920, .height = 1080, .framerate = 60, .bitrate = 20000 };
  config.videoFormat = 1;
  config.dynamicRange = 1;
  config.pre_encode_filter = platf::pre_encode_filter_e::external_neural_enhancement;
  config.frame_pipeline_policy = platf::resolve_frame_pipeline_policy(1, false, true);
  config.frame_pipeline_policy_resolved = true;
  auto backend = boost::make_shared<image_enhancement::backend_use_t>();
  backend->id = "alkaidlab.nvidia_dlssnr";
  backend->path = std::filesystem::path(reinterpret_cast<const char8_t *>(adapter_path));
  backend->runtime_digest = digest;
  config.enhancement_backend = backend;
  auto display = std::make_shared<platf::dxgi::display_ddup_vram_t>();
  ASSERT_EQ(display->init(config, ""), 0);
  ASSERT_TRUE(display->is_hdr()) << "This diagnostic requires an already HDR desktop";
  std::shared_ptr<platf::img_t> frame;
  const auto pull = [&](std::shared_ptr<platf::img_t> &free) {
    free = display->alloc_img();
    return static_cast<bool>(free);
  };
  for (int attempt = 0; attempt < 20 && !frame; ++attempt) {
    const auto status = display->snapshot(pull, frame, std::chrono::milliseconds(50), false);
    display->release_snapshot();
    ASSERT_TRUE(status == platf::capture_e::ok || status == platf::capture_e::timeout);
  }
  ASSERT_TRUE(frame);
  auto encoder = display->make_nvenc_encode_device(platf::pix_fmt_e::p010, config);
  ASSERT_TRUE(encoder);
  ASSERT_TRUE(encoder->init_encoder(config, { video::colorspace_e::bt2020, false, 10 }));
  ASSERT_EQ(encoder->convert(*frame), 0);
  const auto packet = encoder->nvenc->encode_frame(0, true);
  ASSERT_FALSE(packet.data.empty());
  EXPECT_TRUE(packet.idr);
  const auto statuses = video::get_hdr_pipeline_statuses();
  ASSERT_EQ(statuses.size(), 1u);
  const auto &image = static_cast<const platf::dxgi::img_d3d_t &>(*frame);
  EXPECT_EQ(statuses[0].nr_state, image.dummy ? "warming_up" : "active");
  EXPECT_EQ(statuses[0].hdr_mode, "pq");
  // A startup placeholder must not prevent subsequent real HDR frames from
  // enabling NR. Keep the capture deadline bounded and do not inject UI input.
  for (int attempt = 0; attempt < 40 && static_cast<const platf::dxgi::img_d3d_t &>(*frame).dummy; ++attempt) {
    std::shared_ptr<platf::img_t> next;
    const auto status = display->snapshot(pull, next, std::chrono::milliseconds(50), false);
    display->release_snapshot();
    ASSERT_TRUE(status == platf::capture_e::ok || status == platf::capture_e::timeout);
    if (status == platf::capture_e::ok && next) frame = std::move(next);
  }
  ASSERT_FALSE(static_cast<const platf::dxgi::img_d3d_t &>(*frame).dummy) << "No real desktop frame before the diagnostic deadline";
  ASSERT_EQ(encoder->convert(*frame), 0);
  ASSERT_FALSE(encoder->nvenc->encode_frame(1, false).data.empty());
  const auto active_statuses = video::get_hdr_pipeline_statuses();
  ASSERT_EQ(active_statuses.size(), 1u);
  EXPECT_EQ(active_statuses[0].nr_state, "active");
  // Encoded bytes stay in memory and are discarded with the test process.
}
#endif
