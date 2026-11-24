/**
 * @file src/amf/amf_vpe_d3d11.h
 * @brief Experimental AMF VideoConverter backend for Windows frame preparation.
 */
#pragma once

#include <d3d11.h>
#include <memory>
#include <string>

#include "src/video_colorspace.h"

namespace amf {
  class amf_vpe_d3d11 {
  public:
    amf_vpe_d3d11(ID3D11Device *device, ID3D11DeviceContext *immediate,
      const video::sunshine_colorspace_t &colorspace, int width, int height,
      int active_width, int active_height, int offset_x, int offset_y, float hlg_peak_nits = 1000.0f,
      ID3D11ComputeShader *hlg_prepare_shader = nullptr);
    ~amf_vpe_d3d11();

    bool
    available() const;
    const std::string &
    failure_reason() const;
    // The caller holds the capture keyed mutex throughout this synchronous experiment.
    bool
    convert(ID3D11Texture2D *input, ID3D11Texture2D *output, bool linear_fp16);

  private:
    struct impl;
    std::unique_ptr<impl> state;
  };
}  // namespace amf
