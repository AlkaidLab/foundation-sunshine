/**
 * @file src/platform/windows/pre_encode_filter.h
 * @brief Vendor-neutral D3D11 pre-encode filter contract.
 */
#pragma once

#include <memory>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

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

  /// Runtime state of one post-process chain slot (status reporting).
  struct stage_state_t {
    std::string name;
    std::string state { "active" };  // active | bypassed
    std::string failure_reason;
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

    /// Per-slot chain states for /api/runtime/hdr. Composite filters
    /// (chain, failover) forward to their live children; leaf filters keep
    /// the default empty report.
    virtual std::vector<stage_state_t>
    postprocess_stage_states() const {
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

  /**
   * Build the user-configured post-process chain from per-app entries:
   * load each DLL (v2 stage or legacy v1 backend), validate the whole chain
   * against the capture/encoder legs (docs/postprocess_chain.md §5), insert
   * builtin conversions, and wrap the result with the SDR-in-HDR fallback
   * floor. Load failures exclude their stage (logged); a structurally
   * invalid plan degrades to the fallback with the first rule error as the
   * reason. Returns a filter even when no DLL survived (fallback-only).
   */
  std::unique_ptr<pre_encode_filter_t>
  make_configured_postprocess_chain(
    const std::vector<platf::postprocess_stage_entry_t> &entries,
    ID3D11Device *device,
    ID3D11DeviceContext *device_context,
    frame_domain_e capture_domain,
    pixel_encoding_class_e capture_encoding,
    bool hdr_wire,
    const pre_encode_filter_config_t &v1_config);
}  // namespace platf::dxgi
