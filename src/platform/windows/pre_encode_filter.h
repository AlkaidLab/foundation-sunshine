/**
 * @file src/platform/windows/pre_encode_filter.h
 * @brief Vendor-neutral D3D11 pre-encode filter contract.
 */
#pragma once

#include <memory>
#include <filesystem>
#include <string>
#include <string_view>

#include <d3d11.h>

#include "src/platform/frame_contract.h"

namespace platf::dxgi {
  enum class filter_status_e : std::uint8_t {
    ready,
    pending,
    bypass,
    failed,
  };

  struct gpu_frame_view_t {
    ID3D11Texture2D *texture = nullptr;
    ID3D11ShaderResourceView *srv = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    captured_frame_desc_t semantic;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
  };

  struct filter_result_t {
    filter_status_e status = filter_status_e::bypass;
    gpu_frame_view_t frame;
    std::string_view reason;
  };

  class pre_encode_filter_t {
  public:
    virtual ~pre_encode_filter_t() = default;

    virtual bool
    requires_detached_input() const = 0;

    virtual filter_result_t
    process(const gpu_frame_view_t &input) = 0;

    virtual void
    flush() = 0;

    virtual std::string_view
    backend_name() const = 0;

    virtual bool
    degraded() const {
      return false;
    }

    virtual std::string_view
    failure_reason() const {
      return {};
    }
  };

  std::unique_ptr<pre_encode_filter_t>
  make_pre_encode_filter(
    pre_encode_filter_e kind,
    ID3D11Device *device,
    ID3D11DeviceContext *device_context,
    const std::filesystem::path &backend_path = {},
    const pre_encode_filter_config_t &config = {});
}  // namespace platf::dxgi
