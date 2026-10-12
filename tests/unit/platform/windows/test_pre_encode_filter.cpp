#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <d3d11.h>

#include "src/platform/windows/image_enhancement/dlss_nr/hdr_filter.h"
#include "src/platform/windows/pre_encode_filter.h"
#include <cstring>

namespace {
  template <class T>
  struct com_release_t {
    void
    operator()(T *value) const {
      if (value) {
        value->Release();
      }
    }
  };

  template <class T>
  using com_ptr_t = std::unique_ptr<T, com_release_t<T>>;

  struct temporary_directory_t {
    std::filesystem::path path;

    ~temporary_directory_t() {
      std::error_code ignored;
      std::filesystem::remove_all(path, ignored);
    }
  };

  struct d3d_fixture_t {
    com_ptr_t<ID3D11Device> device;
    com_ptr_t<ID3D11DeviceContext> context;

    bool
    init() {
      ID3D11Device *device_raw = nullptr;
      ID3D11DeviceContext *context_raw = nullptr;
      const auto status = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_WARP,
        nullptr,
        0,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &device_raw,
        nullptr,
        &context_raw);
      if (FAILED(status)) {
        return false;
      }
      device.reset(device_raw);
      context.reset(context_raw);
      return true;
    }
  };

  struct input_texture_t {
    com_ptr_t<ID3D11Texture2D> texture;
    com_ptr_t<ID3D11ShaderResourceView> srv;
  };

  input_texture_t
  make_white_input(ID3D11Device *device, std::uint32_t width, std::uint32_t height) {
    std::vector<std::uint32_t> pixels(width * height, 0xFFFFFFFFu);
    D3D11_SUBRESOURCE_DATA initial_data {
      .pSysMem = pixels.data(),
      .SysMemPitch = static_cast<UINT>(width * sizeof(std::uint32_t)),
    };
    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    ID3D11Texture2D *texture_raw = nullptr;
    if (FAILED(device->CreateTexture2D(&desc, &initial_data, &texture_raw))) {
      return {};
    }
    input_texture_t result;
    result.texture.reset(texture_raw);

    ID3D11ShaderResourceView *srv_raw = nullptr;
    if (FAILED(device->CreateShaderResourceView(result.texture.get(), nullptr, &srv_raw))) {
      return {};
    }
    result.srv.reset(srv_raw);
    return result;
  }

  TEST(PreEncodeFilter, NoneDoesNotCreateGpuFilter) {
    d3d_fixture_t d3d;
    ASSERT_TRUE(d3d.init());

    EXPECT_FALSE(platf::dxgi::make_pre_encode_filter(
      platf::pre_encode_filter_e::none,
      d3d.device.get(),
      d3d.context.get()));
  }

  TEST(PreEncodeFilter, MockRequiresDetachedPrivateInput) {
    d3d_fixture_t d3d;
    ASSERT_TRUE(d3d.init());
    auto filter = platf::dxgi::make_pre_encode_filter(
      platf::pre_encode_filter_e::mock_sdr_to_scrgb,
      d3d.device.get(),
      d3d.context.get());
    ASSERT_TRUE(filter);

    auto input = make_white_input(d3d.device.get(), 4, 4);
    ASSERT_TRUE(input.texture);
    platf::dxgi::gpu_frame_view_t view {
      .texture = input.texture.get(),
      .srv = input.srv.get(),
      .format = DXGI_FORMAT_B8G8R8A8_UNORM,
      .semantic = {
        .domain = platf::frame_domain_e::sdr_rec709,
        .encoding = platf::pixel_encoding_class_e::unorm8,
        .reference_white_nits = 80.0f,
        .borrowed = true,
        .source_generation = 1,
      },
      .width = 4,
      .height = 4,
    };

    const auto result = filter->process(view);
    EXPECT_EQ(result.status, platf::dxgi::filter_status_e::failed);
    EXPECT_EQ(result.reason, "input_contract_mismatch");
  }

  TEST(PreEncodeFilter, MockProducesLinearFp16TextureOnGpu) {
    d3d_fixture_t d3d;
    ASSERT_TRUE(d3d.init());
    auto filter = platf::dxgi::make_pre_encode_filter(
      platf::pre_encode_filter_e::mock_sdr_to_scrgb,
      d3d.device.get(),
      d3d.context.get());
    ASSERT_TRUE(filter);

    auto input = make_white_input(d3d.device.get(), 4, 4);
    ASSERT_TRUE(input.texture);
    const platf::dxgi::gpu_frame_view_t view {
      .texture = input.texture.get(),
      .srv = input.srv.get(),
      .format = DXGI_FORMAT_B8G8R8A8_UNORM,
      .semantic = {
        .domain = platf::frame_domain_e::sdr_rec709,
        .encoding = platf::pixel_encoding_class_e::unorm8,
        .reference_white_nits = 80.0f,
        .borrowed = false,
        .source_generation = 9,
      },
      .width = 4,
      .height = 4,
    };

    const auto result = filter->process(view);
    ASSERT_EQ(result.status, platf::dxgi::filter_status_e::ready);
    ASSERT_NE(result.frame.texture, nullptr);
    EXPECT_EQ(result.frame.format, DXGI_FORMAT_R16G16B16A16_FLOAT);
    EXPECT_EQ(result.frame.semantic.domain, platf::frame_domain_e::linear_scrgb);
    EXPECT_EQ(result.frame.semantic.encoding, platf::pixel_encoding_class_e::float16);
    EXPECT_FALSE(result.frame.semantic.borrowed);
    EXPECT_EQ(result.frame.semantic.source_generation, 9u);

    D3D11_TEXTURE2D_DESC output_desc {};
    result.frame.texture->GetDesc(&output_desc);
    output_desc.Usage = D3D11_USAGE_STAGING;
    output_desc.BindFlags = 0;
    output_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    output_desc.MiscFlags = 0;
    ID3D11Texture2D *staging_raw = nullptr;
    ASSERT_TRUE(SUCCEEDED(d3d.device->CreateTexture2D(&output_desc, nullptr, &staging_raw)));
    com_ptr_t<ID3D11Texture2D> staging { staging_raw };
    d3d.context->CopyResource(staging.get(), result.frame.texture);

    D3D11_MAPPED_SUBRESOURCE mapped {};
    ASSERT_TRUE(SUCCEEDED(d3d.context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped)));
    const auto *first_pixel = static_cast<const std::uint16_t *>(mapped.pData);
    EXPECT_EQ(first_pixel[0], 0x3C00u);
    EXPECT_EQ(first_pixel[1], 0x3C00u);
    EXPECT_EQ(first_pixel[2], 0x3C00u);
    EXPECT_EQ(first_pixel[3], 0x3C00u);
    d3d.context->Unmap(staging.get(), 0);
  }

  TEST(PreEncodeFilter, ExternalBackendRunsThroughAdapterLoader) {
    d3d_fixture_t d3d;
    ASSERT_TRUE(d3d.init());
    auto filter = platf::dxgi::make_pre_encode_filter(
      platf::pre_encode_filter_e::external_sdr_to_hdr,
      d3d.device.get(),
      d3d.context.get(),
      std::filesystem::path(FAKE_TRUEHDR_ADAPTER_PATH), {}, "alkaidlab.nvidia_rtx_video");
    ASSERT_TRUE(filter);
    EXPECT_FALSE(filter->degraded());
    EXPECT_EQ(filter->backend_name(), "alkaidlab.nvidia_rtx_video");

    auto input = make_white_input(d3d.device.get(), 4, 4);
    ASSERT_TRUE(input.texture);
    const platf::dxgi::gpu_frame_view_t view {
      .texture = input.texture.get(),
      .srv = input.srv.get(),
      .format = DXGI_FORMAT_B8G8R8A8_UNORM,
      .semantic = {
        .domain = platf::frame_domain_e::sdr_rec709,
        .encoding = platf::pixel_encoding_class_e::unorm8,
        .reference_white_nits = 80.0f,
        .borrowed = false,
        .source_generation = 11,
      },
      .width = 4,
      .height = 4,
    };

    const auto result = filter->process(view);
    ASSERT_EQ(result.status, platf::dxgi::filter_status_e::ready);
    EXPECT_EQ(result.frame.format, DXGI_FORMAT_R16G16B16A16_FLOAT);
    EXPECT_EQ(result.frame.semantic.domain, platf::frame_domain_e::linear_scrgb);
    EXPECT_FALSE(result.frame.semantic.borrowed);
  }

  TEST(PreEncodeFilter, MissingNvidiaRuntimeUsesGpuFallback) {
    d3d_fixture_t d3d;
    ASSERT_TRUE(d3d.init());
    temporary_directory_t test_directory {
      std::filesystem::temp_directory_path() /
        ("sunshine-missing-rtx-runtime-" + std::to_string(GetCurrentProcessId())),
    };
    std::error_code filesystem_error;
    std::filesystem::remove_all(test_directory.path, filesystem_error);
    filesystem_error.clear();
    ASSERT_TRUE(std::filesystem::create_directories(test_directory.path, filesystem_error)) << filesystem_error.message();
    const auto adapter_path = test_directory.path / "foundation_rtx_video_adapter.dll";
    filesystem_error.clear();
    ASSERT_TRUE(std::filesystem::copy_file(
      std::filesystem::path(FAKE_TRUEHDR_ADAPTER_PATH),
      adapter_path,
      std::filesystem::copy_options::overwrite_existing,
      filesystem_error))
      << filesystem_error.message();
    ASSERT_FALSE(std::filesystem::exists(test_directory.path / "nvngx_truehdr.dll"));
    auto filter = platf::dxgi::make_pre_encode_filter(
      platf::pre_encode_filter_e::external_sdr_to_hdr,
      d3d.device.get(),
      d3d.context.get(),
      adapter_path, {}, "alkaidlab.nvidia_rtx_video");
    ASSERT_TRUE(filter);
    EXPECT_TRUE(filter->degraded());
    EXPECT_EQ(filter->backend_name(), "gpu_sdr_in_hdr_fallback");
    EXPECT_EQ(filter->failure_reason(), "runtime_open_failed");
  }

  TEST(PreEncodeFilter, ExternalBackendFailureDegradesToGpuFallback) {
    d3d_fixture_t d3d;
    ASSERT_TRUE(d3d.init());
    auto filter = platf::dxgi::make_pre_encode_filter(
      platf::pre_encode_filter_e::external_sdr_to_hdr,
      d3d.device.get(),
      d3d.context.get(),
      std::filesystem::path(FAKE_TRUEHDR_FAILING_ADAPTER_PATH), {}, "alkaidlab.nvidia_rtx_video");
    ASSERT_TRUE(filter);

    auto input = make_white_input(d3d.device.get(), 4, 4);
    ASSERT_TRUE(input.texture);
    const platf::dxgi::gpu_frame_view_t view {
      .texture = input.texture.get(),
      .srv = input.srv.get(),
      .format = DXGI_FORMAT_B8G8R8A8_UNORM,
      .semantic = {
        .domain = platf::frame_domain_e::sdr_rec709,
        .encoding = platf::pixel_encoding_class_e::unorm8,
        .reference_white_nits = 80.0f,
        .borrowed = false,
        .source_generation = 12,
      },
      .width = 4,
      .height = 4,
    };

    const auto first = filter->process(view);
    ASSERT_EQ(first.status, platf::dxgi::filter_status_e::ready);
    EXPECT_EQ(first.frame.semantic.domain, platf::frame_domain_e::linear_scrgb);
    EXPECT_TRUE(filter->degraded());
    EXPECT_EQ(filter->backend_name(), "gpu_sdr_in_hdr_fallback");
    EXPECT_EQ(filter->failure_reason(), "backend_process_internal_error");

    // The failing primary is discarded for the session; a second frame must
    // remain healthy on the GPU fallback path.
    const auto second = filter->process(view);
    ASSERT_EQ(second.status, platf::dxgi::filter_status_e::ready);
    EXPECT_EQ(second.frame.format, DXGI_FORMAT_R16G16B16A16_FLOAT);
    EXPECT_EQ(second.frame.semantic.source_generation, 12u);
  }
  TEST(PreEncodeFilter, NrBackendWithoutComponentDegradesToIdentityPassthrough) {
    d3d_fixture_t d3d;
    ASSERT_TRUE(d3d.init());
    auto filter = platf::dxgi::make_pre_encode_filter(
      platf::pre_encode_filter_e::external_neural_enhancement,
      d3d.device.get(),
      d3d.context.get(),
      std::filesystem::path(FAKE_TRUEHDR_ADAPTER_PATH).parent_path() / "foundation_dlssnr_adapter.dll",
      {},
      "alkaidlab.nvidia_dlssnr");
    ASSERT_TRUE(filter);
    EXPECT_TRUE(filter->degraded());
    EXPECT_EQ(filter->backend_name(), "identity_neural_passthrough");

    auto input = make_white_input(d3d.device.get(), 4, 4);
    ASSERT_TRUE(input.texture);
    const platf::dxgi::gpu_frame_view_t view {
      .texture = input.texture.get(),
      .srv = input.srv.get(),
      .format = DXGI_FORMAT_B8G8R8A8_UNORM,
      .semantic = {
        .domain = platf::frame_domain_e::sdr_rec709,
        .encoding = platf::pixel_encoding_class_e::unorm8,
        .reference_white_nits = 80.0f,
        .borrowed = false,
        .source_generation = 21,
      },
      .width = 4,
      .height = 4,
    };

    // The fallback is a zero-copy passthrough: the same SDR view comes back
    // untouched so a degraded session keeps encoding captured frames as-is.
    const auto result = filter->process(view);
    ASSERT_EQ(result.status, platf::dxgi::filter_status_e::ready);
    EXPECT_EQ(result.frame.texture, view.texture);
    EXPECT_EQ(result.frame.srv, view.srv);
    EXPECT_EQ(result.frame.format, DXGI_FORMAT_B8G8R8A8_UNORM);
    EXPECT_EQ(result.frame.semantic.domain, platf::frame_domain_e::sdr_rec709);
    EXPECT_EQ(result.frame.semantic.encoding, platf::pixel_encoding_class_e::unorm8);
    EXPECT_FALSE(result.frame.semantic.borrowed);
    EXPECT_EQ(result.frame.semantic.source_generation, 21u);
  }

  TEST(PreEncodeFilter, NrIdentityFallbackStillValidatesTheInputContract) {
    d3d_fixture_t d3d;
    ASSERT_TRUE(d3d.init());
    auto filter = platf::dxgi::make_pre_encode_filter(
      platf::pre_encode_filter_e::external_neural_enhancement,
      d3d.device.get(),
      d3d.context.get(),
      std::filesystem::path(FAKE_TRUEHDR_ADAPTER_PATH).parent_path() / "foundation_dlssnr_adapter.dll",
      {},
      "alkaidlab.nvidia_dlssnr");
    ASSERT_TRUE(filter);

    auto input = make_white_input(d3d.device.get(), 4, 4);
    ASSERT_TRUE(input.texture);
    platf::dxgi::gpu_frame_view_t view {
      .texture = input.texture.get(),
      .srv = input.srv.get(),
      .format = DXGI_FORMAT_B8G8R8A8_UNORM,
      .semantic = {
        .domain = platf::frame_domain_e::sdr_rec709,
        .encoding = platf::pixel_encoding_class_e::unorm8,
        .reference_white_nits = 80.0f,
        .borrowed = true,
        .source_generation = 22,
      },
      .width = 4,
      .height = 4,
    };

    const auto result = filter->process(view);
    EXPECT_EQ(result.status, platf::dxgi::filter_status_e::failed);
    EXPECT_EQ(result.reason, "input_contract_mismatch");
  }
  class proxy_model_t final: public platf::dxgi::pre_encode_filter_t {
  public:
    explicit proxy_model_t(UINT width = 0, UINT height = 0): width_(width), height_(height) {}
    void expect_dimensions(UINT width, UINT height) { width_ = width; height_ = height; }
    void
    flush() override {}
    std::string_view
    backend_name() const override { return "test_proxy_model"; }
    platf::dxgi::filter_result_t
    process(const platf::dxgi::gpu_frame_view_t &input) override {
      EXPECT_EQ(input.semantic.domain, platf::frame_domain_e::sdr_rec709);
      EXPECT_EQ(input.format, DXGI_FORMAT_B8G8R8A8_UNORM);
      if (width_) { EXPECT_EQ(input.width, width_); EXPECT_EQ(input.height, height_); }
      return { .status = platf::dxgi::filter_status_e::ready, .frame = input };
    }
  private:
    UINT width_, height_;
  };

  static void hdr_proxy_identity(int scale) {
    d3d_fixture_t d3d;
    ASSERT_TRUE(d3d.init());
    auto model = std::make_unique<proxy_model_t>();
    auto *observed_model = model.get();
    auto filter = platf::dxgi::image_enhancement::dlss_nr::make_hdr_compatible_filter(
      d3d.device.get(), d3d.context.get(), std::move(model), scale);
    ASSERT_TRUE(filter);
    // FP16 wide-gamut negative, subnormal, 1000-nit and 4000-nit channels.
    const std::uint16_t pattern[] { 0xb000, 0x0001, 0x4a40, 0x3800, 0x5240, 0x3c00, 0x0000, 0x3c00 };
    for (UINT width : { 7u, 19u }) {
      constexpr UINT height = 5;
      observed_model->expect_dimensions(platf::nr_scaled_dimension(width, scale),
        platf::nr_scaled_dimension(height, scale));
      std::vector<std::uint16_t> pixels(width * height * 4);
      for (std::size_t i = 0; i < pixels.size(); ++i) pixels[i] = pattern[i % 8];
      D3D11_TEXTURE2D_DESC desc {};
      desc.Width = width;
      desc.Height = height;
      desc.MipLevels = 1;
      desc.ArraySize = 1;
      desc.SampleDesc.Count = 1;
      desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
      desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      D3D11_SUBRESOURCE_DATA data { pixels.data(), width * 8, 0 };
      ID3D11Texture2D *raw = nullptr;
      ASSERT_TRUE(SUCCEEDED(d3d.device->CreateTexture2D(&desc, &data, &raw)));
      com_ptr_t<ID3D11Texture2D> input(raw);
      ID3D11ShaderResourceView *srv_raw = nullptr;
      ASSERT_TRUE(SUCCEEDED(d3d.device->CreateShaderResourceView(input.get(), nullptr, &srv_raw)));
      com_ptr_t<ID3D11ShaderResourceView> srv(srv_raw);
      platf::dxgi::gpu_frame_view_t view {
        .texture = input.get(),
        .srv = srv.get(),
        .format = desc.Format,
        .semantic = { .domain = platf::frame_domain_e::linear_scrgb,
          .encoding = platf::pixel_encoding_class_e::float16,
          .reference_white_nits = 80,
          .source_generation = 91 },
        .width = width,
        .height = height,
      };
      D3D11_VIEWPORT viewport { 3, 4, 17, 23, 0.1f, 0.9f };
      d3d.context->RSSetViewports(1, &viewport);
      auto result = filter->process(view);
      ASSERT_EQ(result.status, platf::dxgi::filter_status_e::ready) << result.reason;
      EXPECT_EQ(result.frame.semantic.domain, platf::frame_domain_e::linear_scrgb);
      EXPECT_EQ(result.frame.semantic.source_generation, 91u);
      EXPECT_EQ(result.frame.semantic.reference_white_nits, 80);
      UINT count = 1;
      D3D11_VIEWPORT restored {};
      d3d.context->RSGetViewports(&count, &restored);
      EXPECT_EQ(restored.Width, viewport.Width);
      EXPECT_EQ(restored.TopLeftX, viewport.TopLeftX);
      desc.Usage = D3D11_USAGE_STAGING;
      desc.BindFlags = 0;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      ASSERT_TRUE(SUCCEEDED(d3d.device->CreateTexture2D(&desc, nullptr, &raw)));
      com_ptr_t<ID3D11Texture2D> staging(raw);
      d3d.context->CopyResource(staging.get(), result.frame.texture);
      D3D11_MAPPED_SUBRESOURCE mapped {};
      ASSERT_TRUE(SUCCEEDED(d3d.context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped)));
      for (UINT row = 0; row < height; ++row) {
        EXPECT_EQ(std::memcmp(pixels.data() + row * width * 4,
                    static_cast<const char *>(mapped.pData) + row * mapped.RowPitch, width * 8),
          0);
      }
      d3d.context->Unmap(staging.get(), 0);
      // A missing vendor adapter must preserve the HDR frame on its fallback.
      auto unavailable = platf::dxgi::make_pre_encode_filter(
        platf::pre_encode_filter_e::external_neural_enhancement,
        d3d.device.get(), d3d.context.get(), {}, {}, "unavailable");
      ASSERT_TRUE(unavailable);
      auto fallback = unavailable->process(view);
      EXPECT_EQ(fallback.status, platf::dxgi::filter_status_e::ready);
      EXPECT_EQ(fallback.frame.texture, input.get());
      EXPECT_EQ(fallback.frame.semantic.domain, platf::frame_domain_e::linear_scrgb);
    }
  }
  TEST(PreEncodeFilter, HdrProxyIdentityPreservesSignedHighlightsAlphaAndContext) {
    for (int scale : {100, 75, 65, 50, 20}) hdr_proxy_identity(scale);
  }

  TEST(PreEncodeFilter, ScaledSdrIdentityPreservesNativeTextPatternAndDimensions) {
    d3d_fixture_t d3d;
    ASSERT_TRUE(d3d.init());
    constexpr UINT width = 19, height = 7;
    std::vector<std::uint32_t> pixels(width * height);
    for (UINT i = 0; i < pixels.size(); ++i) pixels[i] = i % 2 ? 0xff183fe2 : 0x7fe75b12;
    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = width; desc.Height = height; desc.MipLevels = 1; desc.ArraySize = 1;
    desc.SampleDesc.Count = 1; desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data { pixels.data(), width * 4, 0 };
    ID3D11Texture2D *raw = nullptr;
    ASSERT_TRUE(SUCCEEDED(d3d.device->CreateTexture2D(&desc, &data, &raw)));
    com_ptr_t<ID3D11Texture2D> input(raw);
    ID3D11ShaderResourceView *srv_raw = nullptr;
    ASSERT_TRUE(SUCCEEDED(d3d.device->CreateShaderResourceView(input.get(), nullptr, &srv_raw)));
    com_ptr_t<ID3D11ShaderResourceView> srv(srv_raw);
    platf::dxgi::gpu_frame_view_t view {
      .texture = input.get(), .srv = srv.get(), .format = desc.Format,
      .semantic = { .domain = platf::frame_domain_e::sdr_rec709,
        .encoding = platf::pixel_encoding_class_e::unorm8 },
      .width = width, .height = height,
    };
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ASSERT_TRUE(SUCCEEDED(d3d.device->CreateTexture2D(&desc, nullptr, &raw)));
    com_ptr_t<ID3D11Texture2D> staging(raw);
    for (int scale : {75, 65, 50, 20}) {
      auto filter = platf::dxgi::image_enhancement::dlss_nr::make_hdr_compatible_filter(
        d3d.device.get(), d3d.context.get(), std::make_unique<proxy_model_t>(platf::nr_scaled_dimension(19, scale), platf::nr_scaled_dimension(7, scale)), scale);
      auto result = filter->process(view);
      ASSERT_EQ(result.status, platf::dxgi::filter_status_e::ready) << result.reason;
      EXPECT_EQ(result.frame.width, width); EXPECT_EQ(result.frame.height, height);
      EXPECT_EQ(result.frame.format, desc.Format);
      d3d.context->CopyResource(staging.get(), result.frame.texture);
      D3D11_MAPPED_SUBRESOURCE mapped {};
      ASSERT_TRUE(SUCCEEDED(d3d.context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped)));
      for (UINT row = 0; row < height; ++row) EXPECT_EQ(std::memcmp(pixels.data() + row * width,
        static_cast<const char *>(mapped.pData) + row * mapped.RowPitch, width * 4), 0);
      d3d.context->Unmap(staging.get(), 0);
    }
  }
}  // namespace
