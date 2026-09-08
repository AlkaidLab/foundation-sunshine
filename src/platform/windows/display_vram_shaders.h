/**
 * @file src/platform/windows/display_vram_shaders.h
 * @brief Private D3D11 shader catalog and graphics resource helpers.
 */
#pragma once

#include "display.h"
#include "src/logging.h"
#include <d3dcompiler.h>

namespace platf::dxgi {
  template <class T>
  buf_t
  make_buffer(device_t::pointer device, const T &t) {
    static_assert(sizeof(T) % 16 == 0, "Buffer needs to be aligned on a 16-byte alignment");

    D3D11_BUFFER_DESC buffer_desc {
      sizeof(T),
      D3D11_USAGE_IMMUTABLE,
      D3D11_BIND_CONSTANT_BUFFER
    };

    D3D11_SUBRESOURCE_DATA init_data {
      &t
    };

    buf_t::pointer buf_p;
    auto status = device->CreateBuffer(&buffer_desc, &init_data, &buf_p);
    if (status) {
      BOOST_LOG(error) << "Failed to create buffer: [0x"sv << util::hex(status).to_string_view() << ']';
      return nullptr;
    }

    return buf_t { buf_p };
  }

  blend_t make_blend(device_t::pointer device, bool enable, bool invert);

  extern blob_t convert_yuv420_packed_uv_type0_ps_hlsl;
  extern blob_t convert_yuv420_packed_uv_type0_ps_linear_hlsl;
  extern blob_t convert_yuv420_packed_uv_type0_ps_perceptual_quantizer_hlsl;
  extern blob_t convert_yuv420_packed_uv_type0_ps_hybrid_log_gamma_hlsl;
  extern blob_t convert_yuv420_packed_uv_type0_vs_hlsl;
  extern blob_t convert_yuv420_packed_uv_type0s_ps_hlsl;
  extern blob_t convert_yuv420_packed_uv_type0s_ps_linear_hlsl;
  extern blob_t convert_yuv420_packed_uv_type0s_ps_perceptual_quantizer_hlsl;
  extern blob_t convert_yuv420_packed_uv_type0s_ps_hybrid_log_gamma_hlsl;
  extern blob_t convert_yuv420_packed_uv_type0s_vs_hlsl;
  extern blob_t convert_yuv420_packed_uv_bicubic_ps_hlsl;
  extern blob_t convert_yuv420_packed_uv_bicubic_ps_linear_hlsl;
  extern blob_t convert_yuv420_packed_uv_bicubic_ps_perceptual_quantizer_hlsl;
  extern blob_t convert_yuv420_packed_uv_bicubic_ps_hybrid_log_gamma_hlsl;
  extern blob_t convert_yuv420_packed_uv_bicubic_vs_hlsl;
  extern blob_t convert_yuv420_planar_y_ps_hlsl;
  extern blob_t convert_yuv420_planar_y_ps_linear_hlsl;
  extern blob_t convert_yuv420_planar_y_ps_perceptual_quantizer_hlsl;
  extern blob_t convert_yuv420_planar_y_ps_hybrid_log_gamma_hlsl;
  extern blob_t convert_yuv420_planar_y_vs_hlsl;
  extern blob_t convert_yuv420_planar_y_bicubic_ps_hlsl;
  extern blob_t convert_yuv420_planar_y_bicubic_ps_linear_hlsl;
  extern blob_t convert_yuv420_planar_y_bicubic_ps_perceptual_quantizer_hlsl;
  extern blob_t convert_yuv420_planar_y_bicubic_ps_hybrid_log_gamma_hlsl;
  extern blob_t convert_yuv444_packed_ayuv_ps_hlsl;
  extern blob_t convert_yuv444_packed_ayuv_ps_linear_hlsl;
  extern blob_t convert_yuv444_packed_vs_hlsl;
  extern blob_t convert_yuv444_planar_ps_hlsl;
  extern blob_t convert_yuv444_planar_ps_linear_hlsl;
  extern blob_t convert_yuv444_planar_ps_perceptual_quantizer_hlsl;
  extern blob_t convert_yuv444_planar_ps_hybrid_log_gamma_hlsl;
  extern blob_t convert_yuv444_packed_y410_ps_hlsl;
  extern blob_t convert_yuv444_packed_y410_ps_linear_hlsl;
  extern blob_t convert_yuv444_packed_y410_ps_perceptual_quantizer_hlsl;
  extern blob_t convert_yuv444_packed_y410_ps_hybrid_log_gamma_hlsl;
  extern blob_t convert_yuv444_planar_vs_hlsl;
  extern blob_t cursor_ps_hlsl;
  extern blob_t cursor_ps_normalize_white_hlsl;
  extern blob_t cursor_vs_hlsl;
  extern blob_t simple_cursor_vs_hlsl;
  extern blob_t simple_cursor_ps_hlsl;
  extern blob_t hdr_luminance_analysis_cs_hlsl;
  extern blob_t hdr_luminance_reduce_cs_hlsl;
  extern blob_t convert_yuv420_p010_cs_perceptual_quantizer_hlsl;
  extern blob_t convert_yuv420_p010_cs_hybrid_log_gamma_hlsl;
  extern blob_t convert_yuv420_p010_cs_perceptual_quantizer_hdr_analysis_hlsl;
  extern blob_t convert_yuv420_p010_cs_hybrid_log_gamma_hdr_analysis_hlsl;
  extern blob_t convert_yuv420_nv12_cs_passthrough_hlsl;
  extern blob_t convert_yuv420_nv12_cs_linear_hlsl;
  extern blob_t convert_yuv420_p010_cs_perceptual_quantizer_scaled_hlsl;
  extern blob_t convert_yuv420_p010_cs_hybrid_log_gamma_scaled_hlsl;
  extern blob_t convert_yuv420_p010_cs_perceptual_quantizer_scaled_hdr_analysis_hlsl;
  extern blob_t convert_yuv420_p010_cs_hybrid_log_gamma_scaled_hdr_analysis_hlsl;
  extern blob_t convert_yuv420_nv12_cs_passthrough_scaled_hlsl;
  extern blob_t convert_yuv420_nv12_cs_linear_scaled_hlsl;

  blob_t compile_shader(LPCSTR file, LPCSTR entrypoint, LPCSTR shader_model, const D3D_SHADER_MACRO *defines = nullptr);
  blob_t compile_pixel_shader(LPCSTR file);
  blob_t compile_vertex_shader(LPCSTR file);
  blob_t compile_compute_shader(LPCSTR file, const D3D_SHADER_MACRO *defines = nullptr);
}  // namespace platf::dxgi
