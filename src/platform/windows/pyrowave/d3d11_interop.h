/**
 * @file src/platform/windows/pyrowave/d3d11_interop.h
 * @brief Windows D3D11 resource contract for the experimental PyroWave path.
 */
#pragma once

#include "src/pyrowave/gpu_interop.h"

#include <d3d11.h>

#include <cstdint>

namespace pyrowave::windows {

  struct d3d11_texture_info_t {
    gpu_frame_t frame {};
    LUID adapter_luid {};
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT misc_flags = 0;
    bool shared_handle = false;
    bool keyed_mutex = false;
  };

  /**
   * Inspect a D3D11 texture without transferring ownership of either COM
   * object. The caller keeps the texture and device alive while frame is used.
   */
  [[nodiscard]] interop_result_t
  inspect_d3d11_texture(
    ID3D11Device *device,
    ID3D11Texture2D *texture,
    d3d11_texture_info_t &result) noexcept;

  [[nodiscard]] bool
  matches_adapter(const d3d11_texture_info_t &texture, const device_identity_t &expected) noexcept;

}  // namespace pyrowave::windows
