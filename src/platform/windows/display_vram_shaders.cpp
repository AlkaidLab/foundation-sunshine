/**
 * @file src/platform/windows/display_vram_shaders.cpp
 * @brief D3D11 shader compilation and shared shader catalog initialization.
 */
#include "display_vram_shaders.h"
#include "misc.h"

#if !defined(SUNSHINE_SHADERS_DIR)
#define SUNSHINE_SHADERS_DIR SUNSHINE_ASSETS_DIR "/shaders/directx"
#endif

namespace platf::dxgi {
  blend_t
  make_blend(device_t::pointer device, bool enable, bool invert) {
    D3D11_BLEND_DESC bdesc {};
    auto &rt = bdesc.RenderTarget[0];
    rt.BlendEnable = enable;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    if (enable) {
      rt.BlendOp = D3D11_BLEND_OP_ADD;
      rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;

      if (invert) {
        // Invert colors
        rt.SrcBlend = D3D11_BLEND_INV_DEST_COLOR;
        rt.DestBlend = D3D11_BLEND_INV_SRC_COLOR;
      }
      else {
        // Regular alpha blending
        rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
        rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
      }

      rt.SrcBlendAlpha = D3D11_BLEND_ZERO;
      rt.DestBlendAlpha = D3D11_BLEND_ZERO;
    }

    blend_t blend;
    auto status = device->CreateBlendState(&bdesc, &blend);
    if (status) {
      BOOST_LOG(error) << "Failed to create blend state: [0x"sv << util::hex(status).to_string_view() << ']';
      return nullptr;
    }

    return blend;
  }

  blob_t convert_yuv420_packed_uv_type0_ps_hlsl;
  blob_t convert_yuv420_packed_uv_type0_ps_linear_hlsl;
  blob_t convert_yuv420_packed_uv_type0_ps_perceptual_quantizer_hlsl;
  blob_t convert_yuv420_packed_uv_type0_ps_hybrid_log_gamma_hlsl;
  blob_t convert_yuv420_packed_uv_type0_vs_hlsl;
  blob_t convert_yuv420_packed_uv_type0s_ps_hlsl;
  blob_t convert_yuv420_packed_uv_type0s_ps_linear_hlsl;
  blob_t convert_yuv420_packed_uv_type0s_ps_perceptual_quantizer_hlsl;
  blob_t convert_yuv420_packed_uv_type0s_ps_hybrid_log_gamma_hlsl;
  blob_t convert_yuv420_packed_uv_type0s_vs_hlsl;
  blob_t convert_yuv420_packed_uv_bicubic_ps_hlsl;
  blob_t convert_yuv420_packed_uv_bicubic_ps_linear_hlsl;
  blob_t convert_yuv420_packed_uv_bicubic_ps_perceptual_quantizer_hlsl;
  blob_t convert_yuv420_packed_uv_bicubic_ps_hybrid_log_gamma_hlsl;
  blob_t convert_yuv420_packed_uv_bicubic_vs_hlsl;
  blob_t convert_yuv420_planar_y_ps_hlsl;
  blob_t convert_yuv420_planar_y_ps_linear_hlsl;
  blob_t convert_yuv420_planar_y_ps_perceptual_quantizer_hlsl;
  blob_t convert_yuv420_planar_y_ps_hybrid_log_gamma_hlsl;
  blob_t convert_yuv420_planar_y_vs_hlsl;
  blob_t convert_yuv420_planar_y_bicubic_ps_hlsl;
  blob_t convert_yuv420_planar_y_bicubic_ps_linear_hlsl;
  blob_t convert_yuv420_planar_y_bicubic_ps_perceptual_quantizer_hlsl;
  blob_t convert_yuv420_planar_y_bicubic_ps_hybrid_log_gamma_hlsl;
  blob_t convert_yuv444_packed_ayuv_ps_hlsl;
  blob_t convert_yuv444_packed_ayuv_ps_linear_hlsl;
  blob_t convert_yuv444_packed_vs_hlsl;
  blob_t convert_yuv444_planar_ps_hlsl;
  blob_t convert_yuv444_planar_ps_linear_hlsl;
  blob_t convert_yuv444_planar_ps_perceptual_quantizer_hlsl;
  blob_t convert_yuv444_planar_ps_hybrid_log_gamma_hlsl;
  blob_t convert_yuv444_packed_y410_ps_hlsl;
  blob_t convert_yuv444_packed_y410_ps_linear_hlsl;
  blob_t convert_yuv444_packed_y410_ps_perceptual_quantizer_hlsl;
  blob_t convert_yuv444_packed_y410_ps_hybrid_log_gamma_hlsl;
  blob_t convert_yuv444_planar_vs_hlsl;
  blob_t cursor_ps_hlsl;
  blob_t cursor_ps_normalize_white_hlsl;
  blob_t cursor_vs_hlsl;
  blob_t simple_cursor_vs_hlsl;
  blob_t simple_cursor_ps_hlsl;
  blob_t hdr_luminance_analysis_cs_hlsl;
  blob_t hdr_luminance_reduce_cs_hlsl;
  blob_t convert_yuv420_p010_cs_perceptual_quantizer_hlsl;
  blob_t convert_yuv420_p010_cs_hybrid_log_gamma_hlsl;
  blob_t convert_yuv420_p010_cs_perceptual_quantizer_hdr_analysis_hlsl;
  blob_t convert_yuv420_p010_cs_hybrid_log_gamma_hdr_analysis_hlsl;
  blob_t convert_yuv420_nv12_cs_passthrough_hlsl;
  blob_t convert_yuv420_nv12_cs_linear_hlsl;
  blob_t convert_yuv420_p010_cs_perceptual_quantizer_scaled_hlsl;
  blob_t convert_yuv420_p010_cs_hybrid_log_gamma_scaled_hlsl;
  blob_t convert_yuv420_p010_cs_perceptual_quantizer_scaled_hdr_analysis_hlsl;
  blob_t convert_yuv420_p010_cs_hybrid_log_gamma_scaled_hdr_analysis_hlsl;
  blob_t convert_yuv420_nv12_cs_passthrough_scaled_hlsl;
  blob_t convert_yuv420_nv12_cs_linear_scaled_hlsl;

  blob_t
  compile_shader(
    LPCSTR file,
    LPCSTR entrypoint,
    LPCSTR shader_model,
    const D3D_SHADER_MACRO *defines) {
    blob_t::pointer msg_p = nullptr;
    blob_t::pointer compiled_p;

    DWORD flags = D3DCOMPILE_ENABLE_STRICTNESS;

#ifndef NDEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    auto wFile = from_utf8(file);
    auto status = D3DCompileFromFile(wFile.c_str(), defines, D3D_COMPILE_STANDARD_FILE_INCLUDE, entrypoint, shader_model, flags, 0, &compiled_p, &msg_p);

    if (msg_p) {
      BOOST_LOG(warning) << std::string_view { (const char *) msg_p->GetBufferPointer(), msg_p->GetBufferSize() - 1 };
      msg_p->Release();
    }

    if (status) {
      BOOST_LOG(error) << "Couldn't compile ["sv << file << "] [0x"sv << util::hex(status).to_string_view() << ']';
      return nullptr;
    }

    return blob_t { compiled_p };
  }

  blob_t
  compile_pixel_shader(LPCSTR file) {
    return compile_shader(file, "main_ps", "ps_5_0");
  }

  blob_t
  compile_vertex_shader(LPCSTR file) {
    return compile_shader(file, "main_vs", "vs_5_0");
  }

  blob_t
  compile_compute_shader(LPCSTR file, const D3D_SHADER_MACRO *defines) {
    return compile_shader(file, "main_cs", "cs_5_0", defines);
  }

  int
  init() {
    BOOST_LOG(debug) << "Compiling shaders..."sv;

#define compile_vertex_shader_helper(x) \
  if (!(x##_hlsl = compile_vertex_shader(SUNSHINE_SHADERS_DIR "/" #x ".hlsl"))) return -1;
#define compile_pixel_shader_helper(x) \
  if (!(x##_hlsl = compile_pixel_shader(SUNSHINE_SHADERS_DIR "/" #x ".hlsl"))) return -1;

    compile_pixel_shader_helper(convert_yuv420_packed_uv_type0_ps);
    compile_pixel_shader_helper(convert_yuv420_packed_uv_type0_ps_linear);
    compile_pixel_shader_helper(convert_yuv420_packed_uv_type0_ps_perceptual_quantizer);
    compile_pixel_shader_helper(convert_yuv420_packed_uv_type0_ps_hybrid_log_gamma);
    compile_vertex_shader_helper(convert_yuv420_packed_uv_type0_vs);
    compile_pixel_shader_helper(convert_yuv420_packed_uv_type0s_ps);
    compile_pixel_shader_helper(convert_yuv420_packed_uv_type0s_ps_linear);
    compile_pixel_shader_helper(convert_yuv420_packed_uv_type0s_ps_perceptual_quantizer);
    compile_pixel_shader_helper(convert_yuv420_packed_uv_type0s_ps_hybrid_log_gamma);
    compile_vertex_shader_helper(convert_yuv420_packed_uv_type0s_vs);
    compile_pixel_shader_helper(convert_yuv420_packed_uv_bicubic_ps);
    compile_pixel_shader_helper(convert_yuv420_packed_uv_bicubic_ps_linear);
    compile_pixel_shader_helper(convert_yuv420_packed_uv_bicubic_ps_perceptual_quantizer);
    compile_pixel_shader_helper(convert_yuv420_packed_uv_bicubic_ps_hybrid_log_gamma);
    compile_vertex_shader_helper(convert_yuv420_packed_uv_bicubic_vs);
    compile_pixel_shader_helper(convert_yuv420_planar_y_ps);
    compile_pixel_shader_helper(convert_yuv420_planar_y_ps_linear);
    compile_pixel_shader_helper(convert_yuv420_planar_y_ps_perceptual_quantizer);
    compile_pixel_shader_helper(convert_yuv420_planar_y_ps_hybrid_log_gamma);
    compile_vertex_shader_helper(convert_yuv420_planar_y_vs);
    compile_pixel_shader_helper(convert_yuv420_planar_y_bicubic_ps);
    compile_pixel_shader_helper(convert_yuv420_planar_y_bicubic_ps_linear);
    compile_pixel_shader_helper(convert_yuv420_planar_y_bicubic_ps_perceptual_quantizer);
    compile_pixel_shader_helper(convert_yuv420_planar_y_bicubic_ps_hybrid_log_gamma);
    compile_pixel_shader_helper(convert_yuv444_packed_ayuv_ps);
    compile_pixel_shader_helper(convert_yuv444_packed_ayuv_ps_linear);
    compile_vertex_shader_helper(convert_yuv444_packed_vs);
    compile_pixel_shader_helper(convert_yuv444_planar_ps);
    compile_pixel_shader_helper(convert_yuv444_planar_ps_linear);
    compile_pixel_shader_helper(convert_yuv444_planar_ps_perceptual_quantizer);
    compile_pixel_shader_helper(convert_yuv444_planar_ps_hybrid_log_gamma);
    compile_pixel_shader_helper(convert_yuv444_packed_y410_ps);
    compile_pixel_shader_helper(convert_yuv444_packed_y410_ps_linear);
    compile_pixel_shader_helper(convert_yuv444_packed_y410_ps_perceptual_quantizer);
    compile_pixel_shader_helper(convert_yuv444_packed_y410_ps_hybrid_log_gamma);
    compile_vertex_shader_helper(convert_yuv444_planar_vs);
    compile_pixel_shader_helper(cursor_ps);
    compile_pixel_shader_helper(cursor_ps_normalize_white);
    compile_vertex_shader_helper(cursor_vs);
    compile_pixel_shader_helper(simple_cursor_ps);
    compile_vertex_shader_helper(simple_cursor_vs);

    // Compile HDR luminance analysis compute shaders (optional, non-fatal if fails)
    hdr_luminance_analysis_cs_hlsl = compile_compute_shader(SUNSHINE_SHADERS_DIR "/hdr_luminance_analysis_cs.hlsl");
    if (!hdr_luminance_analysis_cs_hlsl) {
      BOOST_LOG(warning) << "Failed to compile HDR luminance analysis CS, per-frame HDR metadata will use defaults";
    }
    hdr_luminance_reduce_cs_hlsl = compile_compute_shader(SUNSHINE_SHADERS_DIR "/hdr_luminance_reduce_cs.hlsl");
    if (!hdr_luminance_reduce_cs_hlsl) {
      BOOST_LOG(warning) << "Failed to compile HDR luminance reduce CS, per-frame HDR metadata will use defaults";
    }

    // Compile HDR RGB->P010 compute shaders (Phase 1 fast path; non-fatal if fails).
    convert_yuv420_p010_cs_perceptual_quantizer_hlsl = compile_compute_shader(
      SUNSHINE_SHADERS_DIR "/convert_yuv420_p010_cs_perceptual_quantizer.hlsl");
    if (!convert_yuv420_p010_cs_perceptual_quantizer_hlsl) {
      BOOST_LOG(warning) << "Failed to compile P010 PQ compute shader, HDR PQ compute fast path disabled";
    }
    convert_yuv420_p010_cs_hybrid_log_gamma_hlsl = compile_compute_shader(
      SUNSHINE_SHADERS_DIR "/convert_yuv420_p010_cs_hybrid_log_gamma.hlsl");
    if (!convert_yuv420_p010_cs_hybrid_log_gamma_hlsl) {
      BOOST_LOG(warning) << "Failed to compile P010 HLG compute shader, HDR HLG compute fast path disabled";
    }

    const D3D_SHADER_MACRO hdr_analysis_snapshot_defines[] = {
      { "HDR_ANALYSIS_SNAPSHOT", "1" },
      { nullptr, nullptr },
    };
    convert_yuv420_p010_cs_perceptual_quantizer_hdr_analysis_hlsl = compile_compute_shader(
      SUNSHINE_SHADERS_DIR "/convert_yuv420_p010_cs_perceptual_quantizer.hlsl",
      hdr_analysis_snapshot_defines);
    convert_yuv420_p010_cs_hybrid_log_gamma_hdr_analysis_hlsl = compile_compute_shader(
      SUNSHINE_SHADERS_DIR "/convert_yuv420_p010_cs_hybrid_log_gamma.hlsl",
      hdr_analysis_snapshot_defines);
    if (!convert_yuv420_p010_cs_perceptual_quantizer_hdr_analysis_hlsl ||
        !convert_yuv420_p010_cs_hybrid_log_gamma_hdr_analysis_hlsl) {
      BOOST_LOG(warning) << "Failed to compile HDR analysis snapshot compute shader, full-resolution analysis copy fallback will be used";
    }

    // Compile SDR RGB->NV12 compute shaders (Phase 2 fast path; non-fatal if fails).
    convert_yuv420_nv12_cs_passthrough_hlsl = compile_compute_shader(
      SUNSHINE_SHADERS_DIR "/convert_yuv420_nv12_cs_passthrough.hlsl");
    if (!convert_yuv420_nv12_cs_passthrough_hlsl) {
      BOOST_LOG(warning) << "Failed to compile NV12 passthrough compute shader, SDR gamma compute fast path disabled";
    }
    convert_yuv420_nv12_cs_linear_hlsl = compile_compute_shader(
      SUNSHINE_SHADERS_DIR "/convert_yuv420_nv12_cs_linear.hlsl");
    if (!convert_yuv420_nv12_cs_linear_hlsl) {
      BOOST_LOG(warning) << "Failed to compile NV12 linear compute shader, SDR linear compute fast path disabled";
    }

    // Compile scaling variants (Phase 2B). Each uses 5-tap Catmull-Rom-via-bilinear
    // for Y and hardware bilinear for UV; non-fatal if any fails (path stays
    // limited to no-scale for that variant).
    convert_yuv420_p010_cs_perceptual_quantizer_scaled_hlsl = compile_compute_shader(
      SUNSHINE_SHADERS_DIR "/convert_yuv420_p010_cs_perceptual_quantizer_scaled.hlsl");
    if (!convert_yuv420_p010_cs_perceptual_quantizer_scaled_hlsl) {
      BOOST_LOG(warning) << "Failed to compile P010 PQ scaled compute shader, HDR PQ scaled fast path disabled";
    }
    convert_yuv420_p010_cs_hybrid_log_gamma_scaled_hlsl = compile_compute_shader(
      SUNSHINE_SHADERS_DIR "/convert_yuv420_p010_cs_hybrid_log_gamma_scaled.hlsl");
    if (!convert_yuv420_p010_cs_hybrid_log_gamma_scaled_hlsl) {
      BOOST_LOG(warning) << "Failed to compile P010 HLG scaled compute shader, HDR HLG scaled fast path disabled";
    }
    convert_yuv420_p010_cs_perceptual_quantizer_scaled_hdr_analysis_hlsl = compile_compute_shader(
      SUNSHINE_SHADERS_DIR "/convert_yuv420_p010_cs_perceptual_quantizer_scaled.hlsl",
      hdr_analysis_snapshot_defines);
    convert_yuv420_p010_cs_hybrid_log_gamma_scaled_hdr_analysis_hlsl = compile_compute_shader(
      SUNSHINE_SHADERS_DIR "/convert_yuv420_p010_cs_hybrid_log_gamma_scaled.hlsl",
      hdr_analysis_snapshot_defines);
    if (!convert_yuv420_p010_cs_perceptual_quantizer_scaled_hdr_analysis_hlsl ||
        !convert_yuv420_p010_cs_hybrid_log_gamma_scaled_hdr_analysis_hlsl) {
      BOOST_LOG(warning) << "Failed to compile scaled HDR analysis snapshot compute shader, full-resolution analysis copy fallback will be used";
    }
    convert_yuv420_nv12_cs_passthrough_scaled_hlsl = compile_compute_shader(
      SUNSHINE_SHADERS_DIR "/convert_yuv420_nv12_cs_passthrough_scaled.hlsl");
    if (!convert_yuv420_nv12_cs_passthrough_scaled_hlsl) {
      BOOST_LOG(warning) << "Failed to compile NV12 passthrough scaled compute shader, SDR scaled fast path disabled";
    }
    convert_yuv420_nv12_cs_linear_scaled_hlsl = compile_compute_shader(
      SUNSHINE_SHADERS_DIR "/convert_yuv420_nv12_cs_linear_scaled.hlsl");
    if (!convert_yuv420_nv12_cs_linear_scaled_hlsl) {
      BOOST_LOG(warning) << "Failed to compile NV12 linear scaled compute shader, SDR scaled fast path disabled";
    }

    BOOST_LOG(debug) << "Compiled shaders"sv;

#undef compile_vertex_shader_helper
#undef compile_pixel_shader_helper

    return 0;
  }

}  // namespace platf::dxgi
