#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <vector>

#include <d3d11.h>

#include "src/platform/windows/pre_encode_filter.h"

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
    EXPECT_TRUE(filter->requires_detached_input());

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

  TEST(PreEncodeFilter, ExternalBackendRunsThroughVersionedDllAbi) {
    d3d_fixture_t d3d;
    ASSERT_TRUE(d3d.init());
    auto filter = platf::dxgi::make_pre_encode_filter(
      platf::pre_encode_filter_e::external_sdr_to_hdr,
      d3d.device.get(),
      d3d.context.get(),
      std::filesystem::path(FAKE_TRUEHDR_BACKEND_PATH));
    ASSERT_TRUE(filter);
    EXPECT_FALSE(filter->degraded());
    EXPECT_EQ(filter->backend_name(), "external_sdr_to_hdr");

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

  TEST(PreEncodeFilter, ExternalBackendFailureDegradesToGpuFallback) {
    d3d_fixture_t d3d;
    ASSERT_TRUE(d3d.init());
    auto filter = platf::dxgi::make_pre_encode_filter(
      platf::pre_encode_filter_e::external_sdr_to_hdr,
      d3d.device.get(),
      d3d.context.get(),
      std::filesystem::path(FAKE_TRUEHDR_FAILING_BACKEND_PATH));
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
}  // namespace
