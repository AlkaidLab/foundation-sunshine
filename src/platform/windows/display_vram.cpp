/**
 * @file src/platform/windows/display_vram.cpp
 * @brief Definitions for handling video ram.
 */
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>
#include <optional>
#include <cstdlib>
#include <string>

#include <d3dcompiler.h>
#include <directxmath.h>
#include <winuser.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext_d3d11va.h>
}

#include "display.h"
#include "display_cursor.h"
#include "display_vram_internal.h"
#include "misc.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/nvenc/win/nvenc_dynamic_factory.h"
#include "src/amf/amf_d3d11.h"
#include "src/video.h"
#include "src/video_hdr_metadata.h"

#include <AMF/components/DisplayCapture.h>
#include <AMF/core/Factory.h>

#include <boost/algorithm/string/predicate.hpp>

#if !defined(SUNSHINE_SHADERS_DIR)  // for testing this needs to be defined in cmake as we don't do an install
  #define SUNSHINE_SHADERS_DIR SUNSHINE_ASSETS_DIR "/shaders/directx"
#endif
namespace platf {
  using namespace std::literals;
}

static void
free_frame(AVFrame *frame) {
  av_frame_free(&frame);
}

using frame_t = util::safe_ptr<AVFrame, free_frame>;

namespace platf::dxgi {
  using query_t = util::safe_ptr<ID3D11Query, Release<ID3D11Query>>;

  namespace {
    // AMF QUALITY_VBR is 4 for H.264, HEVC, and AV1 in the bundled SDK.
    constexpr auto quality_vbr_rate_control = 4;
    constexpr DWORD vdd_borrow_encoder_acquire_timeout_ms = 16;
    constexpr auto vram_timing_telemetry_interval = std::chrono::seconds(5);
    constexpr uint64_t vram_gpu_timing_sample_interval = 30;
    constexpr size_t vram_gpu_timing_max_pending = 8;

    struct timing_bucket_t {
      uint64_t samples = 0;
      double total_ms = 0.0;
      double min_ms = 0.0;
      double max_ms = 0.0;

      void
      add(double ms) {
        if (samples == 0) {
          min_ms = ms;
          max_ms = ms;
        }
        else {
          min_ms = std::min(min_ms, ms);
          max_ms = std::max(max_ms, ms);
        }
        total_ms += ms;
        ++samples;
      }

      double
      avg_ms() const {
        return samples ? total_ms / static_cast<double>(samples) : 0.0;
      }

      void
      reset() {
        samples = 0;
        total_ms = 0.0;
        min_ms = 0.0;
        max_ms = 0.0;
      }
    };

    double
    gpu_delta_ms(UINT64 begin, UINT64 end, UINT64 frequency) {
      if (end <= begin || frequency == 0) {
        return 0.0;
      }
      return (static_cast<double>(end - begin) * 1000.0) / static_cast<double>(frequency);
    }

    double
    elapsed_ms(std::chrono::steady_clock::duration duration) {
      return static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(duration).count()) / 1000.0;
    }

    bool
    env_flag_enabled(const char *name) {
      const char *raw_value = std::getenv(name);
      if (!raw_value || !*raw_value) {
        return false;
      }

      std::string value { raw_value };
      std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return value == "1" || value == "true" || value == "on" || value == "yes";
    }

    bool
    is_quality_vbr_rate_control(const std::optional<int> &rc_mode) {
      return rc_mode && *rc_mode == quality_vbr_rate_control;
    }
  }  // namespace

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
    const D3D_SHADER_MACRO *defines = nullptr) {
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
  compile_compute_shader(LPCSTR file, const D3D_SHADER_MACRO *defines = nullptr) {
    return compile_shader(file, "main_cs", "cs_5_0", defines);
  }

  class d3d_base_encode_device final {
    struct alignas(16) HlgDisplayParams {
      float peakNits;
      float systemGamma;
      // SDR band re-anchoring: gain applied to scRGB levels at or below
      // sdrBandTopScrgb (Windows SDR white / 80), fading back to 1.0 by 2x.
      // gain 1.0 / top 0 leaves the signal untouched.
      float sdrBandGain;
      float sdrBandTopScrgb;
    };
    static_assert(sizeof(HlgDisplayParams) == 16);

    // Must match the AnalysisParams constant buffer in both HDR analysis shaders.
    struct AnalysisParams {
      uint32_t analysisWidth;
      uint32_t analysisHeight;
      uint32_t sourceWidth;
      uint32_t sourceHeight;
      uint32_t inputHasCellStatistics;
      float maxAnalysisNits;
      uint32_t pad[2];
    };
    static_assert(sizeof(AnalysisParams) == 32);

    struct gpu_timing_sample_t {
      query_t disjoint;
      query_t start;
      query_t after_dispatch;
      query_t before_copy;
      query_t after_copy;
      query_t end;
      bool cs_used = false;
      bool scratch_copy = false;
      bool direct_uav = false;
      bool p010 = false;
      bool scaled = false;
      bool borrowed_vdd = false;
    };

    struct gpu_timing_stats_t {
      timing_bucket_t total;
      timing_bucket_t dispatch;
      timing_bucket_t unbind;
      timing_bucket_t scratch_copy;
      uint64_t cs_samples = 0;
      uint64_t draw_samples = 0;
      uint64_t direct_uav_samples = 0;
      uint64_t scratch_samples = 0;
      uint64_t p010_samples = 0;
      uint64_t scaled_samples = 0;
      uint64_t borrowed_vdd_samples = 0;
      uint64_t disjoint_samples = 0;

      void
      reset() {
        total.reset();
        dispatch.reset();
        unbind.reset();
        scratch_copy.reset();
        cs_samples = 0;
        draw_samples = 0;
        direct_uav_samples = 0;
        scratch_samples = 0;
        p010_samples = 0;
        scaled_samples = 0;
        borrowed_vdd_samples = 0;
        disjoint_samples = 0;
      }
    };

  public:
    ~d3d_base_encode_device() {
      ::video::unregister_hdr_pipeline_status(runtime_status_id);
    }

    bool
    hdr_luminance_analysis_available() const {
      return hdr_analysis_enabled;
    }

    int
    convert(platf::img_t &img_base) {
      if (vram_timing_enabled) {
        poll_gpu_timing_samples();
      }

      // Garbage collect mapped capture images whose weak references have expired
      for (auto it = img_ctx_map.begin(); it != img_ctx_map.end();) {
        if (it->second.img_weak.expired()) {
          it = img_ctx_map.erase(it);
        }
        else {
          it++;
        }
      }

      auto &img = (img_d3d_t &) img_base;
      if (!img.blank) {
        auto &img_ctx = img_ctx_map[img.id];
        const bool can_analyze_hdr_frame = hdr_analysis_enabled && img.linear_gamma && img.format == DXGI_FORMAT_R16G16B16A16_FLOAT;

        // Open the shared capture texture with our ID3D11Device
        if (initialize_image_context(img, img_ctx)) {
          return -1;
        }

        // Poll the previous analysis result before taking the capture mutex.
        if (hdr_analysis_pending) {
          read_hdr_analysis_results();
          if (hdr_luminance_stats_out.valid && !runtime_status.scene_metadata_active) {
            runtime_status.scene_metadata_active = true;
            ::video::update_hdr_pipeline_status(runtime_status_id, runtime_status);
          }
        }

        // Acquire encoder mutex to synchronize with capture code. Normal
        // Sunshine-owned images use key 0; borrowed VDD slots cycle on key 2
        // until the image is reset/destroyed and returned to the producer.
        const auto encoder_acquire_key = img.encoder_acquire_key;
        const auto encoder_release_key = img.encoder_release_key;
        const bool borrowed_vdd_frame = img.borrowed_vdd_frame;
        const DWORD encoder_acquire_timeout = borrowed_vdd_frame ? vdd_borrow_encoder_acquire_timeout_ms : INFINITE;
        const auto acquire_start = vram_timing_enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point {};
        auto status = img_ctx.encoder_mutex->AcquireSync(encoder_acquire_key, encoder_acquire_timeout);
        if (vram_timing_enabled) {
          cpu_acquire_timing.add(elapsed_ms(std::chrono::steady_clock::now() - acquire_start));
        }
        if (status != S_OK) {
          if (borrowed_vdd_frame && status == WAIT_TIMEOUT) {
            BOOST_LOG(warning) << "Failed to acquire encoder mutex key "sv << encoder_acquire_key
                               << " timeout_ms="sv << encoder_acquire_timeout
                               << " borrowed_vdd="sv << borrowed_vdd_frame
                               << " [0x"sv << util::hex(status).to_string_view() << ']';
          }
          else {
            BOOST_LOG(error) << "Failed to acquire encoder mutex key "sv << encoder_acquire_key
                             << " timeout_ms="sv << encoder_acquire_timeout
                             << " borrowed_vdd="sv << borrowed_vdd_frame
                             << " [0x"sv << util::hex(status).to_string_view() << ']';
          }
          // Check if the D3D11 device is lost (TDR, driver crash, etc.)
          if (device.get()) {
            auto removed_reason = device->GetDeviceRemovedReason();
            if (removed_reason != S_OK) {
              BOOST_LOG(error) << "D3D11 device lost during convert, reason: 0x"sv << util::hex(removed_reason).to_string_view();
            }
          }
          if (borrowed_vdd_frame && status == WAIT_TIMEOUT) {
            if (img.abandon_borrowed_vdd_frame(false, 0)) {
              return 0;
            }
          }
          return -1;
        }
        const auto submit_start = vram_timing_enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point {};

        gpu_timing_sample_t gpu_timing_sample;
        gpu_timing_sample_t *gpu_timing = nullptr;
        if (vram_timing_enabled && begin_gpu_timing_sample(gpu_timing_sample)) {
          gpu_timing = &gpu_timing_sample;
          gpu_timing->borrowed_vdd = img.borrowed_vdd_texture;
        }

        auto draw = [&](auto &input, auto &y_or_yuv_viewports, auto &uv_viewport) {
          device_ctx->PSSetShaderResources(0, 1, &input);

          // The SDR white gain buffer may be rebuilt at a frame boundary. Bind
          // the current buffer here so the pixel-shader fallback sees updates
          // just like the compute-shader path.
          if (hlg_display_cbuf) {
            ID3D11Buffer *hlg_cbuf = hlg_display_cbuf.get();
            device_ctx->PSSetConstantBuffers(3, 1, &hlg_cbuf);
          }

          // Select the correct pixel shader based on image gamma type:
          // - linear_gamma AND FP16 format: Use FP16 shader that applies transfer function
          //   (sRGB for SDR, PQ for HDR, HLG for HLG) to convert from linear light
          // - Otherwise: Use standard shader that assumes sRGB gamma input
          //
          // Both conditions are required because:
          // 1. FP16 format + G10/G2084 colorspace = data is truly linear (scRGB)
          // 2. B8G8R8A8 format + G10 colorspace = data was converted TO sRGB by the
          //    capture API (e.g., WGC requests 8-bit format while display is in ACM mode)
          //
          // This prevents double-gamma when the display is in ACM/HDR mode but the
          // capture is in a non-FP16 format, and also when a driver returns FP16 data
          // that already carries sRGB gamma (G22 colorspace with FP16 format).
          const bool use_linear_shader = img.linear_gamma && (img.format == DXGI_FORMAT_R16G16B16A16_FLOAT);

          // Draw Y/YUV
          device_ctx->OMSetRenderTargets(1, &out_Y_or_YUV_rtv, nullptr);
          device_ctx->VSSetShader(convert_Y_or_YUV_vs.get(), nullptr, 0);
          device_ctx->PSSetShader(use_linear_shader ? convert_Y_or_YUV_fp16_ps.get() : convert_Y_or_YUV_ps.get(), nullptr, 0);
          auto viewport_count = (format == DXGI_FORMAT_R16_UINT) ? 3 : 1;
          assert(viewport_count <= y_or_yuv_viewports.size());
          device_ctx->RSSetViewports(viewport_count, y_or_yuv_viewports.data());
          device_ctx->Draw(3 * viewport_count, 0);  // vertex shader will spread vertices across viewports

          // Draw UV if needed
          if (out_UV_rtv) {
            assert(format == DXGI_FORMAT_NV12 || format == DXGI_FORMAT_P010);
            device_ctx->OMSetRenderTargets(1, &out_UV_rtv, nullptr);
            device_ctx->VSSetShader(convert_UV_vs.get(), nullptr, 0);
            device_ctx->PSSetShader(use_linear_shader ? convert_UV_fp16_ps.get() : convert_UV_ps.get(), nullptr, 0);
            device_ctx->RSSetViewports(1, &uv_viewport);
            device_ctx->Draw(3, 0);
          }
        };

        // Clear render target view(s) once so that the aspect ratio mismatch "bars" appear black
        if (!rtvs_cleared) {
          auto black = create_black_texture_for_rtv_clear();
          if (black) draw(black, out_Y_or_YUV_viewports_for_clear, out_UV_viewport_for_clear);
          rtvs_cleared = true;
        }

        // Draw captured frame
        // Try compute-shader fast path first (HDR PQ/HLG -> P010, or SDR -> NV12;
        // type0, no rotation; scaling supported via *_scaled variants).
        const bool input_is_linear_fp16 = img.linear_gamma && (img.format == DXGI_FORMAT_R16G16B16A16_FLOAT);
        const bool hdr_analysis_due =
          can_analyze_hdr_frame && should_dispatch_hdr_analysis();
        bool cs_used = false;
        bool hdr_analysis_snapshot_written = false;
        update_sdr_band_gain();
        if (cs_path_active) {
          if (cs_for_p010) {
            // HDR P010: shader expects linear scRGB FP16 input.
            if (input_is_linear_fp16) {
              const bool write_hdr_analysis_snapshot =
                hdr_analysis_due && hdr_analysis_snapshot_enabled;
              cs_t &shader = write_hdr_analysis_snapshot ?
                               (cs_is_scaled ? cs_p010_scaled_hdr_analysis : cs_p010_hdr_analysis) :
                               (cs_is_scaled ? cs_p010_scaled : cs_p010);
              cs_used = try_dispatch_cs_convert(
                img_ctx.encoder_input_res.get(),
                shader,
                write_hdr_analysis_snapshot,
                gpu_timing);
              hdr_analysis_snapshot_written =
                cs_used && write_hdr_analysis_snapshot;
            }
          } else {
            // SDR NV12: pick variant based on per-frame input format.
            cs_t &shader = input_is_linear_fp16
                             ? (cs_is_scaled ? cs_nv12_linear_scaled : cs_nv12_linear)
                              : (cs_is_scaled ? cs_nv12_pass_scaled : cs_nv12_pass);
            if (shader) {
              cs_used = try_dispatch_cs_convert(
                img_ctx.encoder_input_res.get(), shader, false, gpu_timing);
            }
          }
        }
        if (!cs_used) {
          draw(img_ctx.encoder_input_res, out_Y_or_YUV_viewports, out_UV_viewport);
          mark_draw_gpu_timing(gpu_timing);
        }

        ID3D11ShaderResourceView *emptyShaderResourceView = nullptr;
        device_ctx->PSSetShaderResources(0, 1, &emptyShaderResourceView);

        bool dispatch_hdr_after_unlock = false;
        ID3D11ShaderResourceView *hdr_analysis_srv = nullptr;
        ID3D11ShaderResourceView *hdr_analysis_pq_input_srv = nullptr;
        ID3D11Buffer *hdr_analysis_params = nullptr;

        if (hdr_analysis_due) {
          if (hdr_analysis_snapshot_written) {
            hdr_analysis_srv = hdr_analysis_snapshot_srv.get();
            hdr_analysis_pq_input_srv = hdr_analysis_pq_srv.get();
            hdr_analysis_params = hdr_analysis_snapshot_cbuf.get();
          } else {
            // Fallback for the pixel-shader path and devices that cannot bind the
            // low-resolution snapshot UAV.
            device_ctx->CopyResource(hdr_analysis_input_tex.get(), img_ctx.encoder_texture.get());
            hdr_analysis_srv = hdr_analysis_input_srv.get();
            hdr_analysis_params = hdr_analysis_cbuf.get();
          }
          dispatch_hdr_after_unlock = true;
        }

        // Release encoder mutex to allow capture code to reuse this image.
        finish_gpu_timing_sample(gpu_timing, std::move(gpu_timing_sample));
        if (borrowed_vdd_frame) {
          if (!img.release_borrowed_vdd_after_convert(img_ctx.encoder_mutex.get())) {
            return -1;
          }
        }
        else {
          img_ctx.encoder_mutex->ReleaseSync(encoder_release_key);
        }
        if (vram_timing_enabled) {
          cpu_submit_timing.add(elapsed_ms(std::chrono::steady_clock::now() - submit_start));
          log_cpu_timing();
        }

        if (dispatch_hdr_after_unlock) {
          dispatch_hdr_analysis(hdr_analysis_srv, hdr_analysis_pq_input_srv, hdr_analysis_params);
        }
      }

      return 0;
    }

    void apply_colorspace(const ::video::sunshine_colorspace_t &colorspace) {
      auto color_vectors = ::video::color_vectors_from_colorspace(colorspace, true);

      if (format == DXGI_FORMAT_AYUV ||
          format == DXGI_FORMAT_R16_UINT ||
          format == DXGI_FORMAT_Y410) {
        color_vectors = ::video::color_vectors_from_colorspace(colorspace, false);
      }

      if (!color_vectors) {
        BOOST_LOG(error) << "No vector data for colorspace"sv;
        return;
      }

      auto color_matrix = make_buffer(device.get(), *color_vectors);
      if (!color_matrix) {
        BOOST_LOG(warning) << "Failed to create color matrix"sv;
        return;
      }

      device_ctx->VSSetConstantBuffers(3, 1, &color_matrix);
      device_ctx->PSSetConstantBuffers(0, 1, &color_matrix);
      this->color_matrix = std::move(color_matrix);
    }

    int
    configure_hlg_display(bool use_hlg_shader, bool is_probe) {
      hlg_display_cbuf.reset();
      ID3D11Buffer *null_cbuf = nullptr;
      device_ctx->PSSetConstantBuffers(3, 1, &null_cbuf);
      hlg_sdr_band_gain_value = 1.0f;
      hlg_sdr_band_top_value = 0.0f;

      float analysis_max_nits = 10000.0f;
      if (use_hlg_shader) {
        SS_HDR_METADATA metadata {};
        // Use the effective capture-display metadata as the single source of
        // truth. VDD reports the client-mapped capabilities here; a physical
        // output reports the values after Windows applies its HDR color profile.
        const bool has_display_peak =
          display->get_hdr_metadata(metadata) && metadata.maxDisplayLuminance > 0;
        const float peak_nits = has_display_peak
                                  ? static_cast<float>(metadata.maxDisplayLuminance)
                                  : 1000.0f;
        const float system_gamma = ::video::hlg_system_gamma(peak_nits);
        hlg_peak_nits_value = peak_nits;
        hlg_system_gamma_value = system_gamma;
        const HlgDisplayParams params {
          peak_nits,
          system_gamma,
          1.0f,
          0.0f,
        };

        auto hlg_params = make_buffer(device.get(), params);
        if (!hlg_params) {
          BOOST_LOG(error) << "Failed to create HLG display parameter buffer";
          return -1;
        }

        ID3D11Buffer *hlg_params_p = hlg_params.get();
        device_ctx->PSSetConstantBuffers(3, 1, &hlg_params_p);
        hlg_display_cbuf = std::move(hlg_params);
        // Vivid statistics must describe the encoded HLG range, not scRGB
        // headroom that cannot be represented by the nominal HLG signal.
        analysis_max_nits = std::min(peak_nits, 10000.0f);

        BOOST_LOG(is_probe ? debug : info)
          << "HLG conversion: BT.2100 inverse OOTF, nominal display peak "
          << peak_nits << " nits, system gamma " << system_gamma
          << (has_display_peak
                ? " (capture display metadata)"
                : " (1000-nit fallback)");
      }

      hdr_analysis_max_nits = analysis_max_nits;
      if (hdr_analysis_enabled) {
        const AnalysisParams analysis_params {
          hdr_analysis_width,
          hdr_analysis_height,
          static_cast<uint32_t>(display->width),
          static_cast<uint32_t>(display->height),
          0,
          hdr_analysis_max_nits,
          {},
        };
        auto analysis_cbuf = make_buffer(device.get(), analysis_params);
        if (!analysis_cbuf) {
          BOOST_LOG(warning)
            << "Failed to update HDR analysis luminance limit; disabling dynamic metadata";
          hdr_analysis_enabled = false;
          hdr_analysis_failure_reason = "analysis_setup_failed";
        }
        else {
          hdr_analysis_cbuf = std::move(analysis_cbuf);
        }
      }

      return 0;
    }

    void
    reset_sdr_band_gain() {
      if (!hlg_display_cbuf || hlg_peak_nits_value <= 0.0f ||
          (std::abs(hlg_sdr_band_gain_value - 1.0f) < 0.01f &&
           std::abs(hlg_sdr_band_top_value) < 0.01f)) {
        return;
      }

      const HlgDisplayParams params {
        hlg_peak_nits_value,
        hlg_system_gamma_value,
        1.0f,
        0.0f,
      };
      auto next_buffer = make_buffer(device.get(), params);
      if (!next_buffer) {
        BOOST_LOG(warning) << "Failed to reset HLG SDR band gain; retaining previous value"sv;
        return;
      }
      hlg_display_cbuf = std::move(next_buffer);
      hlg_sdr_band_gain_value = 1.0f;
      hlg_sdr_band_top_value = 0.0f;
    }

    void
    set_client_sdr_white(float nits) {
      client_sdr_white_nits = std::isfinite(nits) && nits > 0.0f ? nits : 0.0f;
      if (client_sdr_white_nits <= 0.0f) {
        reset_sdr_band_gain();
      }
    }

    // Re-anchor SDR-referenced content to the client's SDR reference white.
    // Called per frame before HLG conversion; cheap float compare,
    // the constant buffer is only rebuilt when the gain actually changes.
    void
    update_sdr_band_gain() {
      if (client_sdr_white_nits <= 0.0f) {
        reset_sdr_band_gain();
        return;
      }
      if (!hlg_display_cbuf || hlg_peak_nits_value <= 0.0f) {
        return;
      }
      auto vram_display = std::dynamic_pointer_cast<platf::dxgi::display_vram_t>(display);
      if (!vram_display) {
        return;
      }
      const auto windows_white = vram_display->composed_sdr_white_nits();
      if (!windows_white || *windows_white < 50.0f) {
        return;
      }

      const float gain = std::clamp(client_sdr_white_nits / *windows_white, 0.5f, 2.5f);
      const float band_top = *windows_white / 80.0f;
      if (std::abs(gain - hlg_sdr_band_gain_value) < 0.01f &&
          std::abs(band_top - hlg_sdr_band_top_value) < 0.01f) {
        return;
      }

      const HlgDisplayParams params {
        hlg_peak_nits_value,
        hlg_system_gamma_value,
        gain,
        band_top,
      };
      auto next_buffer = make_buffer(device.get(), params);
      if (!next_buffer) {
        BOOST_LOG(warning) << "Failed to update HLG SDR band gain; retaining previous value"sv;
        return;
      }
      hlg_display_cbuf = std::move(next_buffer);
      hlg_sdr_band_gain_value = gain;
      hlg_sdr_band_top_value = band_top;
      BOOST_LOG(info) << "SDR band gain: phone " << client_sdr_white_nits
                      << " nits, windows " << *windows_white << " nits, gain " << gain;
    }

    int
    init_output(ID3D11Texture2D *frame_texture, int width, int height, const ::video::sunshine_colorspace_t &colorspace, int video_format, bool is_probe = false) {
      ::video::unregister_hdr_pipeline_status(runtime_status_id);
      runtime_status_id = 0;
      hdr_luminance_stats_out = {};
      hdr_analysis_pending = false;
      hdr_analysis_frame_index = 0;
      hdr_analysis_sample_sequence = 0;

      // init() builds the analyzer from the pixel format alone, because the
      // client's colorspace and codec are not known yet at device creation. Both
      // decide whether any dynamic metadata format can actually be carried, so
      // that verdict has to be reached here, before init_compute_path() picks a
      // conversion path based on whether analysis is running.
      //
      // Two independent gates, and analysis is worth running only where they
      // overlap. The stream decides what may describe the content: HLG over AV1
      // allows nothing, because HDR10+ is PQ-only and HDR Vivid has no AV1
      // carriage. The encoder decides what can be written: encoders driven through
      // avcodec never emit HDR Vivid (see the AV_FRAME_DATA_DYNAMIC_HDR_VIVID
      // comment in video.cpp), so HLG leaves them nothing either, even on HEVC.
      // (H.264 never reaches this at all — encoder.h264[DYNAMIC_RANGE] is false
      // unconditionally, so an HDR colorspace is impossible there.)
      const auto stream_formats = ::video::hdr_metadata::formats_for(colorspace, video_format);
      hdr_metadata_formats = stream_formats.intersect(encoder_metadata_formats);
      hdr_analysis_enabled = hdr_analysis_ready && hdr_metadata_formats.any();
      if (hdr_analysis_ready && !hdr_metadata_formats.any()) {
        // Which side vetoed it, so the Web UI can tell "this codec cannot carry it"
        // apart from "this encoder cannot write it".
        hdr_analysis_failure_reason = stream_formats.any() ? "encoder_unsupported" : "format_unsupported";
        BOOST_LOG(is_probe ? debug : info)
          << "HDR luminance analysis disabled: no dynamic metadata format is both allowed by this "
             "transfer function and codec, and writable by this encoder";
      }

      // The underlying frame pool owns the texture, so we must reference it for ourselves
      frame_texture->AddRef();
      output_texture.reset(frame_texture);

      HRESULT status = S_OK;

#define create_vertex_shader_helper(x, y)                                                                    \
  if (FAILED(status = device->CreateVertexShader(x->GetBufferPointer(), x->GetBufferSize(), nullptr, &y))) { \
    BOOST_LOG(error) << "Failed to create vertex shader " << #x << ": " << util::log_hex(status);            \
    return -1;                                                                                               \
  }
#define create_pixel_shader_helper(x, y)                                                                    \
  if (FAILED(status = device->CreatePixelShader(x->GetBufferPointer(), x->GetBufferSize(), nullptr, &y))) { \
    BOOST_LOG(error) << "Failed to create pixel shader " << #x << ": " << util::log_hex(status);            \
    return -1;                                                                                              \
  }

      // Determine which HDR shader to use based on colorspace
      const bool use_pq_shader = ::video::colorspace_is_pq(colorspace);
      const bool use_hlg_shader = ::video::colorspace_is_hlg(colorspace);

      if (configure_hlg_display(use_hlg_shader, is_probe) != 0) {
        return -1;
      }

      const bool downscaling = display->width > width || display->height > height;
      // Determine downscaling quality based on config
      // "fast" = bilinear + 8pt average (original method)
      // "balanced" = bicubic (default, best quality/performance balance)
      // "high_quality" = reserved for future lanczos implementation
      const bool use_bicubic = downscaling && 
                               (config::video.downscaling_quality == "balanced" || 
                                config::video.downscaling_quality == "high_quality");
      
      if (downscaling) {
        if (is_probe) {
          BOOST_LOG(debug) << "Downscaling from " << display->width << "x" << display->height
                           << " to " << width << "x" << height
                           << " using quality: " << config::video.downscaling_quality
                           << (use_bicubic ? " (bicubic)" : " (bilinear+8pt)")
                           << " (encoder probe)";
        }
        else {
          BOOST_LOG(info) << "Downscaling from " << display->width << "x" << display->height
                         << " to " << width << "x" << height
                         << " using quality: " << config::video.downscaling_quality
                         << (use_bicubic ? " (bicubic)" : " (bilinear+8pt)");
        }
      }

      switch (format) {
        case DXGI_FORMAT_NV12:
          // Semi-planar 8-bit YUV 4:2:0
          if (use_bicubic) {
            // Use bicubic sampling for high-quality downscaling
            create_vertex_shader_helper(convert_yuv420_planar_y_vs_hlsl, convert_Y_or_YUV_vs);
            create_pixel_shader_helper(convert_yuv420_planar_y_bicubic_ps_hlsl, convert_Y_or_YUV_ps);
            create_pixel_shader_helper(convert_yuv420_planar_y_bicubic_ps_linear_hlsl, convert_Y_or_YUV_fp16_ps);
            create_vertex_shader_helper(convert_yuv420_packed_uv_bicubic_vs_hlsl, convert_UV_vs);
            create_pixel_shader_helper(convert_yuv420_packed_uv_bicubic_ps_hlsl, convert_UV_ps);
            create_pixel_shader_helper(convert_yuv420_packed_uv_bicubic_ps_linear_hlsl, convert_UV_fp16_ps);
          }
          else {
            create_vertex_shader_helper(convert_yuv420_planar_y_vs_hlsl, convert_Y_or_YUV_vs);
            create_pixel_shader_helper(convert_yuv420_planar_y_ps_hlsl, convert_Y_or_YUV_ps);
            create_pixel_shader_helper(convert_yuv420_planar_y_ps_linear_hlsl, convert_Y_or_YUV_fp16_ps);
            if (downscaling) {
              create_vertex_shader_helper(convert_yuv420_packed_uv_type0s_vs_hlsl, convert_UV_vs);
              create_pixel_shader_helper(convert_yuv420_packed_uv_type0s_ps_hlsl, convert_UV_ps);
              create_pixel_shader_helper(convert_yuv420_packed_uv_type0s_ps_linear_hlsl, convert_UV_fp16_ps);
            }
            else {
              create_vertex_shader_helper(convert_yuv420_packed_uv_type0_vs_hlsl, convert_UV_vs);
              create_pixel_shader_helper(convert_yuv420_packed_uv_type0_ps_hlsl, convert_UV_ps);
              create_pixel_shader_helper(convert_yuv420_packed_uv_type0_ps_linear_hlsl, convert_UV_fp16_ps);
            }
          }
          break;

        case DXGI_FORMAT_P010:
          // Semi-planar 16-bit YUV 4:2:0, 10 most significant bits store the value
          if (use_bicubic) {
            // Use bicubic sampling for high-quality downscaling
            create_vertex_shader_helper(convert_yuv420_planar_y_vs_hlsl, convert_Y_or_YUV_vs);
            create_pixel_shader_helper(convert_yuv420_planar_y_bicubic_ps_hlsl, convert_Y_or_YUV_ps);
            if (use_pq_shader) {
              create_pixel_shader_helper(convert_yuv420_planar_y_bicubic_ps_perceptual_quantizer_hlsl, convert_Y_or_YUV_fp16_ps);
            }
            else if (use_hlg_shader) {
              create_pixel_shader_helper(convert_yuv420_planar_y_bicubic_ps_hybrid_log_gamma_hlsl, convert_Y_or_YUV_fp16_ps);
            }
            else {
              create_pixel_shader_helper(convert_yuv420_planar_y_bicubic_ps_linear_hlsl, convert_Y_or_YUV_fp16_ps);
            }
            create_vertex_shader_helper(convert_yuv420_packed_uv_bicubic_vs_hlsl, convert_UV_vs);
            create_pixel_shader_helper(convert_yuv420_packed_uv_bicubic_ps_hlsl, convert_UV_ps);
            if (use_pq_shader) {
              create_pixel_shader_helper(convert_yuv420_packed_uv_bicubic_ps_perceptual_quantizer_hlsl, convert_UV_fp16_ps);
            }
            else if (use_hlg_shader) {
              create_pixel_shader_helper(convert_yuv420_packed_uv_bicubic_ps_hybrid_log_gamma_hlsl, convert_UV_fp16_ps);
            }
            else {
              create_pixel_shader_helper(convert_yuv420_packed_uv_bicubic_ps_linear_hlsl, convert_UV_fp16_ps);
            }
          }
          else {
            create_vertex_shader_helper(convert_yuv420_planar_y_vs_hlsl, convert_Y_or_YUV_vs);
            create_pixel_shader_helper(convert_yuv420_planar_y_ps_hlsl, convert_Y_or_YUV_ps);
            if (use_pq_shader) {
              create_pixel_shader_helper(convert_yuv420_planar_y_ps_perceptual_quantizer_hlsl, convert_Y_or_YUV_fp16_ps);
            }
            else if (use_hlg_shader) {
              create_pixel_shader_helper(convert_yuv420_planar_y_ps_hybrid_log_gamma_hlsl, convert_Y_or_YUV_fp16_ps);
            }
            else {
              create_pixel_shader_helper(convert_yuv420_planar_y_ps_linear_hlsl, convert_Y_or_YUV_fp16_ps);
            }
            if (downscaling) {
              create_vertex_shader_helper(convert_yuv420_packed_uv_type0s_vs_hlsl, convert_UV_vs);
              create_pixel_shader_helper(convert_yuv420_packed_uv_type0s_ps_hlsl, convert_UV_ps);
              if (use_pq_shader) {
                create_pixel_shader_helper(convert_yuv420_packed_uv_type0s_ps_perceptual_quantizer_hlsl, convert_UV_fp16_ps);
              }
              else if (use_hlg_shader) {
                create_pixel_shader_helper(convert_yuv420_packed_uv_type0s_ps_hybrid_log_gamma_hlsl, convert_UV_fp16_ps);
              }
              else {
                create_pixel_shader_helper(convert_yuv420_packed_uv_type0s_ps_linear_hlsl, convert_UV_fp16_ps);
              }
            }
            else {
              create_vertex_shader_helper(convert_yuv420_packed_uv_type0_vs_hlsl, convert_UV_vs);
              create_pixel_shader_helper(convert_yuv420_packed_uv_type0_ps_hlsl, convert_UV_ps);
              if (use_pq_shader) {
                create_pixel_shader_helper(convert_yuv420_packed_uv_type0_ps_perceptual_quantizer_hlsl, convert_UV_fp16_ps);
              }
              else if (use_hlg_shader) {
                create_pixel_shader_helper(convert_yuv420_packed_uv_type0_ps_hybrid_log_gamma_hlsl, convert_UV_fp16_ps);
              }
              else {
                create_pixel_shader_helper(convert_yuv420_packed_uv_type0_ps_linear_hlsl, convert_UV_fp16_ps);
              }
            }
          }
          break;

        case DXGI_FORMAT_R16_UINT:
          // Planar 16-bit YUV 4:4:4, 10 most significant bits store the value
          create_vertex_shader_helper(convert_yuv444_planar_vs_hlsl, convert_Y_or_YUV_vs);
          create_pixel_shader_helper(convert_yuv444_planar_ps_hlsl, convert_Y_or_YUV_ps);
          if (use_pq_shader) {
            create_pixel_shader_helper(convert_yuv444_planar_ps_perceptual_quantizer_hlsl, convert_Y_or_YUV_fp16_ps);
          }
          else if (use_hlg_shader) {
            create_pixel_shader_helper(convert_yuv444_planar_ps_hybrid_log_gamma_hlsl, convert_Y_or_YUV_fp16_ps);
          }
          else {
            create_pixel_shader_helper(convert_yuv444_planar_ps_linear_hlsl, convert_Y_or_YUV_fp16_ps);
          }
          break;

        case DXGI_FORMAT_AYUV:
          // Packed 8-bit YUV 4:4:4
          create_vertex_shader_helper(convert_yuv444_packed_vs_hlsl, convert_Y_or_YUV_vs);
          create_pixel_shader_helper(convert_yuv444_packed_ayuv_ps_hlsl, convert_Y_or_YUV_ps);
          create_pixel_shader_helper(convert_yuv444_packed_ayuv_ps_linear_hlsl, convert_Y_or_YUV_fp16_ps);
          break;

        case DXGI_FORMAT_Y410:
          // Packed 10-bit YUV 4:4:4
          create_vertex_shader_helper(convert_yuv444_packed_vs_hlsl, convert_Y_or_YUV_vs);
          create_pixel_shader_helper(convert_yuv444_packed_y410_ps_hlsl, convert_Y_or_YUV_ps);
          if (use_pq_shader) {
            create_pixel_shader_helper(convert_yuv444_packed_y410_ps_perceptual_quantizer_hlsl, convert_Y_or_YUV_fp16_ps);
          }
          else if (use_hlg_shader) {
            create_pixel_shader_helper(convert_yuv444_packed_y410_ps_hybrid_log_gamma_hlsl, convert_Y_or_YUV_fp16_ps);
          }
          else {
            create_pixel_shader_helper(convert_yuv444_packed_y410_ps_linear_hlsl, convert_Y_or_YUV_fp16_ps);
          }
          break;

        default:
          BOOST_LOG(error) << "Unable to create shaders because of the unrecognized surface format";
          return -1;
      }

#undef create_vertex_shader_helper
#undef create_pixel_shader_helper

      auto out_width = width;
      auto out_height = height;

      float in_width = display->width;
      float in_height = display->height;

      // Ensure aspect ratio is maintained
      auto scalar = std::fminf(out_width / in_width, out_height / in_height);
      auto out_width_f = in_width * scalar;
      auto out_height_f = in_height * scalar;

      // result is always positive
      auto offsetX = (out_width - out_width_f) / 2;
      auto offsetY = (out_height - out_height_f) / 2;

      out_Y_or_YUV_viewports[0] = { offsetX, offsetY, out_width_f, out_height_f, 0.0f, 1.0f };  // Y plane
      out_Y_or_YUV_viewports[1] = out_Y_or_YUV_viewports[0];  // U plane
      out_Y_or_YUV_viewports[1].TopLeftY += out_height;
      out_Y_or_YUV_viewports[2] = out_Y_or_YUV_viewports[1];  // V plane
      out_Y_or_YUV_viewports[2].TopLeftY += out_height;

      out_Y_or_YUV_viewports_for_clear[0] = { 0, 0, (float) out_width, (float) out_height, 0.0f, 1.0f };  // Y plane
      out_Y_or_YUV_viewports_for_clear[1] = out_Y_or_YUV_viewports_for_clear[0];  // U plane
      out_Y_or_YUV_viewports_for_clear[1].TopLeftY += out_height;
      out_Y_or_YUV_viewports_for_clear[2] = out_Y_or_YUV_viewports_for_clear[1];  // V plane
      out_Y_or_YUV_viewports_for_clear[2].TopLeftY += out_height;

      out_UV_viewport = { offsetX / 2, offsetY / 2, out_width_f / 2, out_height_f / 2, 0.0f, 1.0f };
      out_UV_viewport_for_clear = { 0, 0, (float) out_width / 2, (float) out_height / 2, 0.0f, 1.0f };

      float subsample_offset_in[16 / sizeof(float)] { 1.0f / (float) out_width_f, 1.0f / (float) out_height_f };  // aligned to 16-byte
      subsample_offset = make_buffer(device.get(), subsample_offset_in);

      if (!subsample_offset) {
        BOOST_LOG(error) << "Failed to create subsample offset vertex constant buffer";
        return -1;
      }
      device_ctx->VSSetConstantBuffers(0, 1, &subsample_offset);

      {
        int32_t rotation_modifier = display->display_rotation == DXGI_MODE_ROTATION_UNSPECIFIED ? 0 : display->display_rotation - 1;
        int32_t rotation_data[16 / sizeof(int32_t)] { -rotation_modifier };  // aligned to 16-byte
        auto rotation = make_buffer(device.get(), rotation_data);
        if (!rotation) {
          BOOST_LOG(error) << "Failed to create display rotation vertex constant buffer";
          return -1;
        }
        device_ctx->VSSetConstantBuffers(1, 1, &rotation);
      }

      DXGI_FORMAT rtv_Y_or_YUV_format = DXGI_FORMAT_UNKNOWN;
      DXGI_FORMAT rtv_UV_format = DXGI_FORMAT_UNKNOWN;
      bool rtv_simple_clear = false;

      switch (format) {
        case DXGI_FORMAT_NV12:
          rtv_Y_or_YUV_format = DXGI_FORMAT_R8_UNORM;
          rtv_UV_format = DXGI_FORMAT_R8G8_UNORM;
          rtv_simple_clear = true;
          break;

        case DXGI_FORMAT_P010:
          rtv_Y_or_YUV_format = DXGI_FORMAT_R16_UNORM;
          rtv_UV_format = DXGI_FORMAT_R16G16_UNORM;
          rtv_simple_clear = true;
          break;

        case DXGI_FORMAT_AYUV:
          rtv_Y_or_YUV_format = DXGI_FORMAT_R8G8B8A8_UINT;
          break;

        case DXGI_FORMAT_R16_UINT:
          rtv_Y_or_YUV_format = DXGI_FORMAT_R16_UINT;
          break;

        case DXGI_FORMAT_Y410:
          rtv_Y_or_YUV_format = DXGI_FORMAT_R10G10B10A2_UINT;
          break;

        default:
          BOOST_LOG(error) << "Unable to create render target views because of the unrecognized surface format";
          return -1;
      }

      auto create_rtv = [&](auto &rt, DXGI_FORMAT rt_format) -> bool {
        D3D11_RENDER_TARGET_VIEW_DESC rtv_desc = {};
        rtv_desc.Format = rt_format;
        rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;

        auto status = device->CreateRenderTargetView(output_texture.get(), &rtv_desc, &rt);
        if (FAILED(status)) {
          BOOST_LOG(error) << "Failed to create render target view: " << util::log_hex(status);
          return false;
        }

        return true;
      };

      // Create Y/YUV render target view
      if (!create_rtv(out_Y_or_YUV_rtv, rtv_Y_or_YUV_format)) return -1;

      // Create UV render target view if needed
      if (rtv_UV_format != DXGI_FORMAT_UNKNOWN && !create_rtv(out_UV_rtv, rtv_UV_format)) return -1;

      if (rtv_simple_clear) {
        // Clear the RTVs to ensure the aspect ratio padding is black
        const float y_black[] = { 0.0f, 0.0f, 0.0f, 0.0f };
        device_ctx->ClearRenderTargetView(out_Y_or_YUV_rtv.get(), y_black);
        if (out_UV_rtv) {
          const float uv_black[] = { 0.5f, 0.5f, 0.5f, 0.5f };
          device_ctx->ClearRenderTargetView(out_UV_rtv.get(), uv_black);
        }
        rtvs_cleared = true;
      }
      else {
        // Can't use ClearRenderTargetView(), will clear on first convert()
        rtvs_cleared = false;
      }

      // Try to enable the compute-shader fast path for HDR P010 (Phase 1).
      // Falls back to PS silently if any precondition or capability check fails.
      {
        // Use rounding (not truncation) so near-integer floats map correctly
        // and stay consistent with the viewports derived from the same values.
        int active_w = static_cast<int>(std::lround(out_width_f));
        int active_h = static_cast<int>(std::lround(out_height_f));
        int active_off_x = static_cast<int>(std::lround(offsetX));
        int active_off_y = static_cast<int>(std::lround(offsetY));
        init_compute_path(out_width, out_height, active_w, active_h, active_off_x, active_off_y, colorspace);
      }

      publish_runtime_status(colorspace, is_probe);
      return 0;
    }

    int
    init(
      std::shared_ptr<platf::display_t> display,
      adapter_t::pointer adapter_p,
      pix_fmt_e pix_fmt,
      ::video::hdr_metadata::formats_t supported_formats) {
      encoder_metadata_formats = supported_formats;
      switch (pix_fmt) {
        case pix_fmt_e::nv12:
          format = DXGI_FORMAT_NV12;
          break;

        case pix_fmt_e::p010:
          format = DXGI_FORMAT_P010;
          break;

        case pix_fmt_e::ayuv:
          format = DXGI_FORMAT_AYUV;
          break;

        case pix_fmt_e::yuv444p16:
          format = DXGI_FORMAT_R16_UINT;
          break;

        case pix_fmt_e::y410:
          format = DXGI_FORMAT_Y410;
          break;

        default:
          BOOST_LOG(error) << "D3D11 backend doesn't support pixel format: " << from_pix_fmt(pix_fmt);
          return -1;
      }

      D3D_FEATURE_LEVEL featureLevels[] {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_3,
        D3D_FEATURE_LEVEL_9_2,
        D3D_FEATURE_LEVEL_9_1
      };

      HRESULT status = D3D11CreateDevice(
        adapter_p,
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        D3D11_CREATE_DEVICE_FLAGS | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        featureLevels, sizeof(featureLevels) / sizeof(D3D_FEATURE_LEVEL),
        D3D11_SDK_VERSION,
        &device,
        nullptr,
        &device_ctx);

      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to create encoder D3D11 device [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }

      dxgi::dxgi_t dxgi;
      status = device->QueryInterface(IID_IDXGIDevice, (void **) &dxgi);
      if (FAILED(status)) {
        BOOST_LOG(warning) << "Failed to query DXGI interface from device [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }

      status = dxgi->SetGPUThreadPriority(7);
      if (FAILED(status)) {
        BOOST_LOG(warning) << "Failed to increase encoding GPU thread priority. Please run application as administrator for optimal performance.";
      }

      auto default_color_vectors = ::video::color_vectors_from_colorspace({::video::colorspace_e::rec601, false, 8}, true);
      if (!default_color_vectors) {
        BOOST_LOG(error) << "Missing color vectors for Rec. 601"sv;
        return -1;
      }

      color_matrix = make_buffer(device.get(), *default_color_vectors);
      if (!color_matrix) {
        BOOST_LOG(error) << "Failed to create color matrix buffer"sv;
        return -1;
      }
      device_ctx->VSSetConstantBuffers(3, 1, &color_matrix);
      device_ctx->PSSetConstantBuffers(0, 1, &color_matrix);

      this->display = std::dynamic_pointer_cast<display_base_t>(display);
      if (!this->display) {
        return -1;
      }
      display = nullptr;

      blend_disable = make_blend(device.get(), false, false);
      if (!blend_disable) {
        return -1;
      }

      D3D11_SAMPLER_DESC sampler_desc {};
      sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
      sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
      sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
      sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
      sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
      sampler_desc.MinLOD = 0;
      sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;

      status = device->CreateSamplerState(&sampler_desc, &sampler_linear);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to create linear sampler state [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }

      device_ctx->OMSetBlendState(blend_disable.get(), nullptr, 0xFFFFFFFFu);
      // s0 = linear (existing shaders), s1 = point (high-quality resampling shaders)
      sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
      status = device->CreateSamplerState(&sampler_desc, &sampler_point);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to create point sampler state [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }

      ID3D11SamplerState *samplers[] = { sampler_linear.get(), sampler_point.get() };
      device_ctx->PSSetSamplers(0, 2, samplers);
      device_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

      // Initialize HDR luminance analyzer for HDR formats (P010, Y410, R16_UINT)
      // The analyzer is optional — if it fails, HDR will still work with static metadata only
      hdr_analysis_failure_reason.clear();
      const bool hdr_format =
        format == DXGI_FORMAT_P010 || format == DXGI_FORMAT_Y410 || format == DXGI_FORMAT_R16_UINT;
      if (hdr_format && config::video.hdr_luminance_analysis != "off") {
        if (!encoder_metadata_formats.any()) {
          hdr_analysis_failure_reason = "encoder_unsupported";
          // Without this the analyzer simply never appears in the log, which reads
          // exactly like a working setup that produces no metadata.
          BOOST_LOG(info) << "HDR luminance analysis requested but this encode device does not "
                             "carry dynamic metadata; static metadata only";
        }
        else if (init_hdr_luminance_analyzer() != 0) {
          hdr_analysis_failure_reason = "analysis_setup_failed";
          BOOST_LOG(warning) << "HDR luminance analyzer init failed, dynamic metadata will use defaults";
        }
      }

      return 0;
    }

    struct encoder_img_ctx_t {
      // Used to determine if the underlying texture changes.
      // Not safe for actual use by the encoder!
      texture2d_t::const_pointer capture_texture_p;

      texture2d_t encoder_texture;
      shader_res_t encoder_input_res;
      keyed_mutex_t encoder_mutex;

      std::weak_ptr<const platf::img_t> img_weak;

      void
      reset() {
        capture_texture_p = nullptr;
        encoder_texture.reset();
        encoder_input_res.reset();
        encoder_mutex.reset();
        img_weak.reset();
      }
    };

    int
    initialize_image_context(const img_d3d_t &img, encoder_img_ctx_t &img_ctx) {
      // If we've already opened the shared texture, we're done
      if (img_ctx.encoder_texture && img.capture_texture.get() == img_ctx.capture_texture_p) {
        return 0;
      }

      // Reset this image context in case it was used before with a different texture.
      // Textures can change when transitioning from a dummy image to a real image.
      img_ctx.reset();

      device1_t device1;
      auto status = device->QueryInterface(__uuidof(ID3D11Device1), (void **) &device1);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to query ID3D11Device1 [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }

      // Open a handle to the shared texture
      status = device1->OpenSharedResource1(img.encoder_texture_handle, __uuidof(ID3D11Texture2D), (void **) &img_ctx.encoder_texture);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to open shared image texture [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }

      // Get the keyed mutex to synchronize with the capture code
      status = img_ctx.encoder_texture->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **) &img_ctx.encoder_mutex);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to query IDXGIKeyedMutex [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }

      // Create the SRV for the encoder texture
      status = device->CreateShaderResourceView(img_ctx.encoder_texture.get(), nullptr, &img_ctx.encoder_input_res);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to create shader resource view for encoding [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }

      img_ctx.capture_texture_p = img.capture_texture.get();

      img_ctx.img_weak = img.weak_from_this();

      return 0;
    }

    query_t
    make_query(D3D11_QUERY query) {
      D3D11_QUERY_DESC desc = {};
      desc.Query = query;

      ID3D11Query *query_p = nullptr;
      const auto status = device->CreateQuery(&desc, &query_p);
      if (FAILED(status)) {
        BOOST_LOG(debug) << "[vram] GPU timing query creation failed [0x"sv
                         << util::hex(status).to_string_view() << ']';
        return nullptr;
      }

      return query_t { query_p };
    }

    bool
    begin_gpu_timing_sample(gpu_timing_sample_t &sample) {
      if (gpu_timing_disabled) {
        return false;
      }
      ++gpu_timing_frame_counter;
      if ((gpu_timing_frame_counter % vram_gpu_timing_sample_interval) != 0 ||
          gpu_timing_pending.size() >= vram_gpu_timing_max_pending) {
        return false;
      }

      sample.disjoint = make_query(D3D11_QUERY_TIMESTAMP_DISJOINT);
      sample.start = make_query(D3D11_QUERY_TIMESTAMP);
      sample.after_dispatch = make_query(D3D11_QUERY_TIMESTAMP);
      sample.before_copy = make_query(D3D11_QUERY_TIMESTAMP);
      sample.after_copy = make_query(D3D11_QUERY_TIMESTAMP);
      sample.end = make_query(D3D11_QUERY_TIMESTAMP);
      if (!sample.disjoint || !sample.start || !sample.after_dispatch ||
          !sample.before_copy || !sample.after_copy || !sample.end) {
        gpu_timing_disabled = true;
        return false;
      }

      device_ctx->Begin(sample.disjoint.get());
      device_ctx->End(sample.start.get());
      return true;
    }

    void
    mark_draw_gpu_timing(gpu_timing_sample_t *timing) {
      if (!timing) {
        return;
      }
      timing->cs_used = false;
      timing->scratch_copy = false;
      timing->direct_uav = false;
      timing->p010 = false;
      timing->scaled = false;
      device_ctx->End(timing->after_dispatch.get());
      device_ctx->End(timing->before_copy.get());
      device_ctx->End(timing->after_copy.get());
    }

    void
    finish_gpu_timing_sample(gpu_timing_sample_t *timing, gpu_timing_sample_t &&sample) {
      if (!timing) {
        return;
      }

      device_ctx->End(timing->end.get());
      device_ctx->End(timing->disjoint.get());
      gpu_timing_pending.emplace_back(std::move(sample));
    }

    void
    poll_gpu_timing_samples() {
      while (!gpu_timing_pending.empty()) {
        auto &sample = gpu_timing_pending.front();
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint = {};
        HRESULT status = device_ctx->GetData(sample.disjoint.get(), &disjoint, sizeof(disjoint), 0);
        if (status != S_OK) {
          break;
        }

        UINT64 start = 0;
        UINT64 after_dispatch = 0;
        UINT64 before_copy = 0;
        UINT64 after_copy = 0;
        UINT64 end = 0;
        const bool ready =
          device_ctx->GetData(sample.start.get(), &start, sizeof(start), 0) == S_OK &&
          device_ctx->GetData(sample.after_dispatch.get(), &after_dispatch, sizeof(after_dispatch), 0) == S_OK &&
          device_ctx->GetData(sample.before_copy.get(), &before_copy, sizeof(before_copy), 0) == S_OK &&
          device_ctx->GetData(sample.after_copy.get(), &after_copy, sizeof(after_copy), 0) == S_OK &&
          device_ctx->GetData(sample.end.get(), &end, sizeof(end), 0) == S_OK;
        if (!ready) {
          break;
        }

        if (!disjoint.Disjoint && disjoint.Frequency != 0) {
          gpu_timing_stats.total.add(gpu_delta_ms(start, end, disjoint.Frequency));
          if (sample.cs_used) {
            ++gpu_timing_stats.cs_samples;
            if (sample.direct_uav) {
              ++gpu_timing_stats.direct_uav_samples;
            }
            if (sample.scratch_copy) {
              ++gpu_timing_stats.scratch_samples;
            }
            if (sample.p010) {
              ++gpu_timing_stats.p010_samples;
            }
            if (sample.scaled) {
              ++gpu_timing_stats.scaled_samples;
            }
            if (sample.borrowed_vdd) {
              ++gpu_timing_stats.borrowed_vdd_samples;
            }
            gpu_timing_stats.dispatch.add(gpu_delta_ms(start, after_dispatch, disjoint.Frequency));
            gpu_timing_stats.unbind.add(gpu_delta_ms(after_dispatch, before_copy, disjoint.Frequency));
            if (sample.scratch_copy) {
              gpu_timing_stats.scratch_copy.add(gpu_delta_ms(before_copy, after_copy, disjoint.Frequency));
            }
          }
          else {
            ++gpu_timing_stats.draw_samples;
          }
        }
        else {
          ++gpu_timing_stats.disjoint_samples;
        }

        gpu_timing_pending.pop_front();
      }

      log_gpu_timing();
    }

    void
    log_gpu_timing() {
      const auto now = std::chrono::steady_clock::now();
      if (gpu_timing_last_log.time_since_epoch().count() == 0) {
        gpu_timing_last_log = now;
        return;
      }
      if (now - gpu_timing_last_log < vram_timing_telemetry_interval) {
        return;
      }
      if (gpu_timing_stats.total.samples == 0 && gpu_timing_stats.disjoint_samples == 0) {
        return;
      }

      BOOST_LOG(info) << "[vram] GPU timing: samples="sv << gpu_timing_stats.total.samples
                      << " cs="sv << gpu_timing_stats.cs_samples
                      << " draw="sv << gpu_timing_stats.draw_samples
                      << " direct_uav="sv << gpu_timing_stats.direct_uav_samples
                      << " scratch="sv << gpu_timing_stats.scratch_samples
                      << " p010="sv << gpu_timing_stats.p010_samples
                      << " scaled="sv << gpu_timing_stats.scaled_samples
                      << " borrowed_vdd="sv << gpu_timing_stats.borrowed_vdd_samples
                      << " pending="sv << gpu_timing_pending.size()
                      << " disjoint="sv << gpu_timing_stats.disjoint_samples
                      << " total_ms="sv << gpu_timing_stats.total.min_ms
                      << "/"sv << gpu_timing_stats.total.avg_ms()
                      << "/"sv << gpu_timing_stats.total.max_ms
                      << " dispatch_ms="sv << gpu_timing_stats.dispatch.min_ms
                      << "/"sv << gpu_timing_stats.dispatch.avg_ms()
                      << "/"sv << gpu_timing_stats.dispatch.max_ms
                      << " unbind_ms="sv << gpu_timing_stats.unbind.min_ms
                      << "/"sv << gpu_timing_stats.unbind.avg_ms()
                      << "/"sv << gpu_timing_stats.unbind.max_ms
                      << " scratch_copy_ms="sv << gpu_timing_stats.scratch_copy.min_ms
                      << "/"sv << gpu_timing_stats.scratch_copy.avg_ms()
                      << "/"sv << gpu_timing_stats.scratch_copy.max_ms;
      gpu_timing_stats.reset();
      gpu_timing_last_log = now;
    }

    void
    log_cpu_timing() {
      const auto now = std::chrono::steady_clock::now();
      if (cpu_timing_last_log.time_since_epoch().count() == 0) {
        cpu_timing_last_log = now;
        return;
      }
      if (now - cpu_timing_last_log < vram_timing_telemetry_interval) {
        return;
      }
      if (cpu_acquire_timing.samples == 0 && cpu_submit_timing.samples == 0) {
        return;
      }

      BOOST_LOG(info) << "[vram] CPU timing: samples="sv << cpu_submit_timing.samples
                      << " encoder_mutex_wait_ms="sv << cpu_acquire_timing.min_ms
                      << "/"sv << cpu_acquire_timing.avg_ms()
                      << "/"sv << cpu_acquire_timing.max_ms
                      << " command_submit_ms="sv << cpu_submit_timing.min_ms
                      << "/"sv << cpu_submit_timing.avg_ms()
                      << "/"sv << cpu_submit_timing.max_ms;
      cpu_acquire_timing.reset();
      cpu_submit_timing.reset();
      cpu_timing_last_log = now;
    }

    shader_res_t
    create_black_texture_for_rtv_clear() {
      constexpr auto width = 32;
      constexpr auto height = 32;

      D3D11_TEXTURE2D_DESC texture_desc = {};
      texture_desc.Width = width;
      texture_desc.Height = height;
      texture_desc.MipLevels = 1;
      texture_desc.ArraySize = 1;
      texture_desc.SampleDesc.Count = 1;
      texture_desc.Usage = D3D11_USAGE_IMMUTABLE;
      texture_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
      texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

      std::vector<uint8_t> mem(4 * width * height, 0);
      D3D11_SUBRESOURCE_DATA texture_data = { mem.data(), 4 * width, 0 };

      texture2d_t texture;
      auto status = device->CreateTexture2D(&texture_desc, &texture_data, &texture);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to create black texture: " << util::log_hex(status);
        return {};
      }

      shader_res_t resource_view;
      status = device->CreateShaderResourceView(texture.get(), nullptr, &resource_view);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to create black texture resource view: " << util::log_hex(status);
        return {};
      }

      return resource_view;
    }

    void
    publish_runtime_status(
      const ::video::sunshine_colorspace_t &colorspace,
      bool is_probe) {
      if (is_probe) {
        ::video::unregister_hdr_pipeline_status(runtime_status_id);
        runtime_status_id = 0;
        return;
      }

      const bool use_pq = ::video::colorspace_is_pq(colorspace);
      const bool use_hlg = ::video::colorspace_is_hlg(colorspace);
      runtime_status.hdr_mode = use_pq ? "pq" : use_hlg ? "hlg" : "sdr";
      runtime_status.analysis_mode = config::video.hdr_luminance_analysis;
      runtime_status.analysis_active = (use_pq || use_hlg) && hdr_analysis_enabled;
      runtime_status.scene_metadata_active = false;
      runtime_status.metadata_formats.clear();
      if (runtime_status.analysis_active) {
        // Report what the stream can actually carry rather than inferring it from
        // the transfer function: HDR Vivid has no AV1 carriage, so advertising it
        // there described metadata the encoder had already stopped emitting.
        if (hdr_metadata_formats.hdr10plus) {
          runtime_status.metadata_formats.emplace_back("hdr10_plus");
        }
        if (hdr_metadata_formats.vivid) {
          runtime_status.metadata_formats.emplace_back("hdr_vivid");
        }
      }

      runtime_status.conversion_path =
        cs_path_active
          ? (cs_writes_output_directly
               ? "compute_shader_direct"
               : "compute_shader_scratch")
          : "pixel_shader";
      runtime_status.conversion_fallback_reason =
        cs_path_active ? std::string {} : cs_fallback_reason;
      runtime_status.analysis_failure_reason =
        runtime_status.analysis_active ? std::string {} : hdr_analysis_failure_reason;

      if (runtime_status_id == 0) {
        runtime_status_id = ::video::register_hdr_pipeline_status(runtime_status);
      }
      else {
        ::video::update_hdr_pipeline_status(runtime_status_id, runtime_status);
      }
    }

    ::video::color_t *color_p;

    buf_t subsample_offset;
    buf_t color_matrix;
    buf_t hlg_display_cbuf;

    // Client-reported SDR reference white (nits); 0 = not reported, gain stays 1.
    float client_sdr_white_nits = 0.0f;
    float hlg_peak_nits_value = 0.0f;
    float hlg_system_gamma_value = 1.2f;
    float hlg_sdr_band_gain_value = 1.0f;
    float hlg_sdr_band_top_value = 0.0f;

    blend_t blend_disable;
    sampler_state_t sampler_linear;
    sampler_state_t sampler_point;

    render_target_t out_Y_or_YUV_rtv;
    render_target_t out_UV_rtv;
    bool rtvs_cleared = false;

    // d3d_img_t::id -> encoder_img_ctx_t
    // These store the encoder textures for each img_t that passes through
    // convert(). We can't store them in the img_t itself because it is shared
    // amongst multiple hwdevice_t objects (and therefore multiple ID3D11Devices).
    std::map<uint32_t, encoder_img_ctx_t> img_ctx_map;

    std::shared_ptr<display_base_t> display;

    vs_t convert_Y_or_YUV_vs;
    ps_t convert_Y_or_YUV_ps;
    ps_t convert_Y_or_YUV_fp16_ps;

    vs_t convert_UV_vs;
    ps_t convert_UV_ps;
    ps_t convert_UV_fp16_ps;

    std::array<D3D11_VIEWPORT, 3> out_Y_or_YUV_viewports, out_Y_or_YUV_viewports_for_clear;
    D3D11_VIEWPORT out_UV_viewport, out_UV_viewport_for_clear;

    DXGI_FORMAT format;

    device_t device;
    device_ctx_t device_ctx;

    texture2d_t output_texture;

    std::deque<gpu_timing_sample_t> gpu_timing_pending;
    gpu_timing_stats_t gpu_timing_stats;
    timing_bucket_t cpu_acquire_timing;
    timing_bucket_t cpu_submit_timing;
    std::chrono::steady_clock::time_point gpu_timing_last_log {};
    std::chrono::steady_clock::time_point cpu_timing_last_log {};
    uint64_t gpu_timing_frame_counter = 0;
    bool vram_timing_enabled = env_flag_enabled("SUNSHINE_VRAM_TIMING");
    bool gpu_timing_disabled = false;

    // ===== HDR Luminance Analyzer (Two-Pass GPU Reduction) =====
    // Pass 1: Per-tile CS — each 16x16 group produces {min, max, sum, count}
    // Pass 2: Single-group CS — reduces all groups to one final result on GPU
    // CPU only reads 1 FinalResult (no iteration over thousands of groups)
    cs_t hdr_pass1_cs;                     // First pass: per-tile analysis
    cs_t hdr_pass2_cs;                     // Second pass: global reduction
    texture2d_t hdr_analysis_input_tex;    // Dedicated copy of the HDR frame for analysis outside the keyed mutex
    shader_res_t hdr_analysis_input_srv;   // SRV for the copied HDR frame
    texture2d_t hdr_analysis_snapshot_tex; // Capped per-cell scalar statistics from the P010 converter
    shader_res_t hdr_analysis_snapshot_srv;
    uav_t hdr_analysis_snapshot_uav;
    texture2d_t hdr_analysis_pq_tex;       // Per-cell average PQ-coded maxRGB, same grid
    shader_res_t hdr_analysis_pq_srv;
    uav_t hdr_analysis_pq_uav;
    buf_t hdr_group_results_buf;           // Pass 1 output (default usage + UAV + SRV)
    uav_t hdr_group_results_uav;           // UAV view for pass 1 output
    shader_res_t hdr_group_results_srv;    // SRV view for pass 2 input
    buf_t hdr_final_result_buf;            // Pass 2 output (default usage + UAV)
    uav_t hdr_final_result_uav;            // UAV view for pass 2 output
    buf_t hdr_global_histogram_buf;        // 256-bin PQ histogram accumulated by pass 1 atomics
    uav_t hdr_global_histogram_uav;        // Typed R32_UINT UAV (clearable + atomic-capable)
    buf_t hdr_staging_buf;                 // Staging buffer for CPU readback (1 FinalResult)
    buf_t hdr_analysis_cbuf;               // Constant buffer for pass 1 (analysis resolution)
    buf_t hdr_analysis_snapshot_cbuf;      // Shared converter/pass 1 params for the snapshot
    buf_t hdr_reduce_cbuf;                 // Constant buffer for pass 2 (numGroups)
    uint32_t hdr_analysis_width = 0;       // Analysis grid width (downsampled from source)
    uint32_t hdr_analysis_height = 0;      // Analysis grid height (downsampled from source)
    uint32_t hdr_num_groups = 0;           // Number of thread groups dispatched in pass 1
    uint64_t hdr_analysis_frame_index = 0; // Used to downsample analysis frequency
    uint64_t hdr_analysis_sample_sequence = 0; // Counts completed, independent GPU samples
    bool hdr_analysis_pending = false;     // Whether we have results ready to read
    bool hdr_analysis_ready = false;       // Whether the analyzer's GPU resources were created
    bool hdr_analysis_enabled = false;     // Whether analysis runs: resources exist and the stream can carry metadata
    ::video::hdr_metadata::formats_t hdr_metadata_formats;  // Dynamic metadata formats this stream may carry
    bool hdr_analysis_snapshot_enabled = false; // P010 converter fills the private analysis texture
    float hdr_analysis_max_nits = 10000.0f; // Clamp metadata to the encoded transfer-function range
    std::string hdr_analysis_failure_reason;

    // ===== Compute-shader RGB->P010/NV12 fast path =====
    // Phase 1: HDR PQ/HLG -> P010. Phase 2A: SDR sRGB/scRGB -> NV12.
    // Phase 2B: same with scaling (active rect != source size), via 5-tap
    // Catmull-Rom-via-bilinear (Y) + hardware bilinear box (UV) sampler path.
    cs_t cs_p010;                          // HDR P010 converter, no-scale (PQ or HLG)
    cs_t cs_p010_hdr_analysis;             // HDR P010 converter + low-resolution analysis snapshot
    cs_t cs_nv12_pass;                     // SDR NV12 converter, no-scale, sRGB BGRA8 input
    cs_t cs_nv12_linear;                   // SDR NV12 converter, no-scale, linear scRGB FP16 input
    cs_t cs_p010_scaled;                   // HDR P010 converter, scaling (PQ or HLG)
    cs_t cs_p010_scaled_hdr_analysis;      // Scaled HDR P010 converter + analysis snapshot
    cs_t cs_nv12_pass_scaled;              // SDR NV12 converter, scaling, sRGB BGRA8 input
    cs_t cs_nv12_linear_scaled;            // SDR NV12 converter, scaling, linear scRGB FP16 input
    texture2d_t cs_scratch_tex;            // Scratch texture (UAV-bindable). Empty when writing directly to output_texture.
    uav_t cs_y_uav;                        // Y plane UAV (R8_UNORM for NV12, R16_UNORM for P010)
    uav_t cs_uv_uav;                       // UV plane UAV (R8G8_UNORM for NV12, R16G16_UNORM for P010)
    buf_t cs_layout_cbuf;                  // Layout cbuffer (b1) for CS
    int  cs_dispatch_groups_x = 0;         // Dispatch dims over the active rect (saves work in letterbox case)
    int  cs_dispatch_groups_y = 0;
    int  cs_copy_w = 0;                    // Logical copy width (Intel QSV may back output_texture with a larger padded surface)
    int  cs_copy_h = 0;                    // Logical copy height (ditto)
    bool cs_path_active = false;           // True when CS conversion path is initialized for this output
    bool cs_use_pq = false;                // Selected transfer function (PQ or HLG) for HDR variant
    bool cs_for_p010 = false;              // True for HDR P010 path, false for SDR NV12 path
    bool cs_is_scaled = false;             // True when active rect != source (use *_scaled variants)
    bool cs_writes_output_directly = false; // True when UAV is bound directly to output_texture (no scratch + CopyResource)
    std::string cs_fallback_reason;

    std::uint64_t runtime_status_id = 0;
    ::video::hdr_pipeline_status_t runtime_status;
    // What this encode device can actually write into the bitstream, independent
    // of what the stream would allow. Set at init(); intersected with the stream's
    // own verdict in init_output().
    ::video::hdr_metadata::formats_t encoder_metadata_formats { .hdr10plus = true, .vivid = true };

    // Must match HLSL GroupResult layout exactly
    static constexpr uint32_t HISTOGRAM_BINS = 256;
    static constexpr uint32_t HDR_ANALYSIS_INTERVAL = 4;
    static constexpr uint32_t HDR_ANALYSIS_MAX_WIDTH = 1920;
    static constexpr uint32_t HDR_ANALYSIS_MAX_HEIGHT = 1080;

    // Pass 1 per-tile output. Deliberately scalars only: the PQ histogram is
    // accumulated straight into a single global buffer by sparse atomics in pass 1,
    // instead of being carried per-tile and merged by pass 2. Carrying it here cost
    // hundreds of bytes per tile and would force pass 2 to walk a large sparse
    // array from a single thread group.
    struct GroupResult {
      float minMaxRGB;
      float maxMaxRGB;
      float sumMaxRGB;
      float sumMaxRGB_PQ;
      uint32_t pixelCount;
    };

    // Must match HLSL FinalResult layout exactly. This one keeps the histogram because
    // it is what the CPU reads back.
    struct FinalResult {
      float minMaxRGB;
      float maxMaxRGB;
      float sumMaxRGB;
      float sumMaxRGB_PQ;
      uint32_t pixelCount;
      uint32_t histogram[HISTOGRAM_BINS];
    };

    bool
    should_dispatch_hdr_analysis() {
      const bool should_dispatch = (hdr_analysis_frame_index % HDR_ANALYSIS_INTERVAL) == 0;
      ++hdr_analysis_frame_index;
      return should_dispatch;
    }

    /**
     * @brief Initialize the two-pass HDR luminance analysis compute pipeline.
     * Pass 1: Per-tile analysis CS (dispatched per-frame)
     * Pass 2: Single-group reduction CS (dispatched per-frame, reduces all groups to 1 result)
     * @return 0 on success, -1 on failure (non-fatal, analysis will be disabled)
     */
    int
    init_hdr_luminance_analyzer() {
      if (!hdr_luminance_analysis_cs_hlsl || !hdr_luminance_reduce_cs_hlsl) {
        BOOST_LOG(warning) << "HDR luminance analysis CS not compiled, skipping init";
        return -1;
      }

      // Create pass 1 compute shader (per-tile analysis)
      HRESULT status = device->CreateComputeShader(
        hdr_luminance_analysis_cs_hlsl->GetBufferPointer(),
        hdr_luminance_analysis_cs_hlsl->GetBufferSize(),
        nullptr,
        &hdr_pass1_cs);
      if (FAILED(status)) {
        BOOST_LOG(warning) << "Failed to create HDR pass 1 compute shader: " << util::log_hex(status);
        return -1;
      }

      // Create pass 2 compute shader (global reduction)
      status = device->CreateComputeShader(
        hdr_luminance_reduce_cs_hlsl->GetBufferPointer(),
        hdr_luminance_reduce_cs_hlsl->GetBufferSize(),
        nullptr,
        &hdr_pass2_cs);
      if (FAILED(status)) {
        BOOST_LOG(warning) << "Failed to create HDR pass 2 compute shader: " << util::log_hex(status);
        return -1;
      }

      // Analyze at a capped resolution to keep dynamic HDR metadata cheap enough
      // to run in the capture path.
      uint32_t width = display->width;
      uint32_t height = display->height;
      float scale_x = static_cast<float>(HDR_ANALYSIS_MAX_WIDTH) / static_cast<float>(width);
      float scale_y = static_cast<float>(HDR_ANALYSIS_MAX_HEIGHT) / static_cast<float>(height);
      float analysis_scale = std::fmin(1.0f, std::fmin(scale_x, scale_y));
      hdr_analysis_width = std::max<uint32_t>(1, static_cast<uint32_t>(width * analysis_scale + 0.5f));
      hdr_analysis_height = std::max<uint32_t>(1, static_cast<uint32_t>(height * analysis_scale + 0.5f));

      uint32_t groups_x = (hdr_analysis_width + 15) / 16;
      uint32_t groups_y = (hdr_analysis_height + 15) / 16;
      hdr_num_groups = groups_x * groups_y;

      // --- Dedicated HDR analysis input copy ---
      D3D11_TEXTURE2D_DESC analysis_input_desc = {};
      analysis_input_desc.Width = width;
      analysis_input_desc.Height = height;
      analysis_input_desc.MipLevels = 1;
      analysis_input_desc.ArraySize = 1;
      analysis_input_desc.SampleDesc.Count = 1;
      analysis_input_desc.Usage = D3D11_USAGE_DEFAULT;
      analysis_input_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
      analysis_input_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

      status = device->CreateTexture2D(&analysis_input_desc, nullptr, &hdr_analysis_input_tex);
      if (FAILED(status)) {
        BOOST_LOG(warning) << "Failed to create HDR analysis input texture: " << util::log_hex(status);
        return -1;
      }

      status = device->CreateShaderResourceView(hdr_analysis_input_tex.get(), nullptr, &hdr_analysis_input_srv);
      if (FAILED(status)) {
        BOOST_LOG(warning) << "Failed to create HDR analysis input SRV: " << util::log_hex(status);
        return -1;
      }

      // --- Constant buffer for pass 1 (analysis resolution) ---
      AnalysisParams analysis_cb_data = {
        hdr_analysis_width,
        hdr_analysis_height,
        width,
        height,
        0,
        hdr_analysis_max_nits,
        {},
      };
      hdr_analysis_cbuf = make_buffer(device.get(), analysis_cb_data);
      if (!hdr_analysis_cbuf) {
        BOOST_LOG(warning) << "Failed to create HDR analysis constant buffer";
        return -1;
      }

      // --- Pass 1 output: structured buffer with UAV + SRV ---
      D3D11_BUFFER_DESC buf_desc = {};
      buf_desc.ByteWidth = hdr_num_groups * sizeof(GroupResult);
      buf_desc.Usage = D3D11_USAGE_DEFAULT;
      buf_desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
      buf_desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
      buf_desc.StructureByteStride = sizeof(GroupResult);

      status = device->CreateBuffer(&buf_desc, nullptr, &hdr_group_results_buf);
      if (FAILED(status)) {
        BOOST_LOG(warning) << "Failed to create HDR group results buffer: " << util::log_hex(status);
        return -1;
      }

      // UAV for pass 1 output
      D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
      uav_desc.Format = DXGI_FORMAT_UNKNOWN;
      uav_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
      uav_desc.Buffer.NumElements = hdr_num_groups;

      status = device->CreateUnorderedAccessView(hdr_group_results_buf.get(), &uav_desc, &hdr_group_results_uav);
      if (FAILED(status)) {
        BOOST_LOG(warning) << "Failed to create HDR group UAV: " << util::log_hex(status);
        return -1;
      }

      // SRV for pass 2 input (read group results)
      D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
      srv_desc.Format = DXGI_FORMAT_UNKNOWN;
      srv_desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
      srv_desc.Buffer.NumElements = hdr_num_groups;

      status = device->CreateShaderResourceView(hdr_group_results_buf.get(), &srv_desc, &hdr_group_results_srv);
      if (FAILED(status)) {
        BOOST_LOG(warning) << "Failed to create HDR group SRV: " << util::log_hex(status);
        return -1;
      }

      // --- Pass 2 output: single FinalResult ---
      D3D11_BUFFER_DESC final_desc = {};
      final_desc.ByteWidth = sizeof(FinalResult);
      final_desc.Usage = D3D11_USAGE_DEFAULT;
      final_desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
      final_desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
      final_desc.StructureByteStride = sizeof(FinalResult);

      status = device->CreateBuffer(&final_desc, nullptr, &hdr_final_result_buf);
      if (FAILED(status)) {
        BOOST_LOG(warning) << "Failed to create HDR final result buffer: " << util::log_hex(status);
        return -1;
      }

      D3D11_UNORDERED_ACCESS_VIEW_DESC final_uav_desc = {};
      final_uav_desc.Format = DXGI_FORMAT_UNKNOWN;
      final_uav_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
      final_uav_desc.Buffer.NumElements = 1;

      status = device->CreateUnorderedAccessView(hdr_final_result_buf.get(), &final_uav_desc, &hdr_final_result_uav);
      if (FAILED(status)) {
        BOOST_LOG(warning) << "Failed to create HDR final UAV: " << util::log_hex(status);
        return -1;
      }

      // --- Global PQ-domain histogram accumulated by pass 1 via InterlockedAdd ---
      // Typed (non-structured) R32_UINT so ClearUnorderedAccessViewUint() is well-defined
      // on it; a structured-buffer UAV is not reliably clearable across drivers.
      D3D11_BUFFER_DESC hist_desc = {};
      hist_desc.ByteWidth = HISTOGRAM_BINS * sizeof(uint32_t);
      hist_desc.Usage = D3D11_USAGE_DEFAULT;
      hist_desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;

      status = device->CreateBuffer(&hist_desc, nullptr, &hdr_global_histogram_buf);
      if (FAILED(status)) {
        BOOST_LOG(warning) << "Failed to create HDR global histogram buffer: " << util::log_hex(status);
        return -1;
      }

      D3D11_UNORDERED_ACCESS_VIEW_DESC hist_uav_desc = {};
      hist_uav_desc.Format = DXGI_FORMAT_R32_UINT;
      hist_uav_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
      hist_uav_desc.Buffer.NumElements = HISTOGRAM_BINS;

      status = device->CreateUnorderedAccessView(hdr_global_histogram_buf.get(), &hist_uav_desc, &hdr_global_histogram_uav);
      if (FAILED(status)) {
        BOOST_LOG(warning) << "Failed to create HDR global histogram UAV: " << util::log_hex(status);
        return -1;
      }

      // --- Constant buffer for pass 2 (numGroups) ---
      D3D11_BUFFER_DESC cb_desc = {};
      cb_desc.ByteWidth = 16;  // 16-byte aligned: uint numGroups + 12 bytes padding
      cb_desc.Usage = D3D11_USAGE_IMMUTABLE;
      cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

      struct {
        uint32_t numGroups;
        uint32_t pad[3];
      } cb_data = { hdr_num_groups, {} };

      D3D11_SUBRESOURCE_DATA cb_init = {};
      cb_init.pSysMem = &cb_data;

      status = device->CreateBuffer(&cb_desc, &cb_init, &hdr_reduce_cbuf);
      if (FAILED(status)) {
        BOOST_LOG(warning) << "Failed to create HDR reduce constant buffer: " << util::log_hex(status);
        return -1;
      }

      // --- Staging buffer for async CPU readback (1 FinalResult only) ---
      D3D11_BUFFER_DESC staging_desc = {};
      staging_desc.ByteWidth = sizeof(FinalResult);
      staging_desc.Usage = D3D11_USAGE_STAGING;
      staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

      status = device->CreateBuffer(&staging_desc, nullptr, &hdr_staging_buf);
      if (FAILED(status)) {
        BOOST_LOG(warning) << "Failed to create HDR staging buffer: " << util::log_hex(status);
        return -1;
      }

      // Resources exist; whether they get used is init_output()'s call, once the
      // colorspace and codec are known.
      hdr_analysis_ready = true;
      BOOST_LOG(info) << "HDR luminance analyzer initialized (two-pass): " << width << "x" << height
                      << ", analysis " << hdr_analysis_width << "x" << hdr_analysis_height
                      << ", " << hdr_num_groups << " groups (" << groups_x << "x" << groups_y << ")"
                      << ", interval 1/" << HDR_ANALYSIS_INTERVAL
                      << ", staging: " << sizeof(FinalResult) << " bytes";
      return 0;
    }

    /**
     * @brief Dispatch the two-pass luminance analysis for the current frame.
     * Pass 1: Per-tile analysis — reads scRGB texture, writes per-group results
     * Pass 2: Global reduction — reads per-group results, writes 1 final result
     * Then copies final result to staging for async CPU readback next frame.
     * @param input_srv SRV of either the scRGB FP16 frame or pre-aggregated snapshot
     * @param cell_pq_srv Per-cell PQ average that accompanies a snapshot input, else null
     * @param analysis_params Parameters describing that input
     */
    void
    dispatch_hdr_analysis(
      ID3D11ShaderResourceView *input_srv,
      ID3D11ShaderResourceView *cell_pq_srv,
      ID3D11Buffer *analysis_params) {
      if (!hdr_analysis_enabled || !input_srv || !analysis_params) return;

      // Unbind render targets to avoid resource hazard (SRV vs RTV conflict)
      ID3D11RenderTargetView *null_rtv = nullptr;
      device_ctx->OMSetRenderTargets(1, &null_rtv, nullptr);

      // The global histogram accumulates across the whole frame, so it must start at zero.
      const UINT hist_clear[4] = { 0, 0, 0, 0 };
      device_ctx->ClearUnorderedAccessViewUint(hdr_global_histogram_uav.get(), hist_clear);

      // ===== Pass 1: Per-tile analysis =====
      device_ctx->CSSetShader(hdr_pass1_cs.get(), nullptr, 0);
      // t1 stays unbound on the full-frame fallback, which computes the PQ sum itself.
      ID3D11ShaderResourceView *pass1_srvs[] = { input_srv, cell_pq_srv };
      device_ctx->CSSetShaderResources(0, 2, pass1_srvs);
      ID3D11UnorderedAccessView *pass1_uavs[] = { hdr_group_results_uav.get(), hdr_global_histogram_uav.get() };
      device_ctx->CSSetUnorderedAccessViews(0, 2, pass1_uavs, nullptr);
      device_ctx->CSSetConstantBuffers(0, 1, &analysis_params);

      uint32_t groups_x = (hdr_analysis_width + 15) / 16;
      uint32_t groups_y = (hdr_analysis_height + 15) / 16;
      device_ctx->Dispatch(groups_x, groups_y, 1);

      // Unbind pass 1 resources
      ID3D11ShaderResourceView *null_srv = nullptr;
      ID3D11ShaderResourceView *null_srvs[2] = { nullptr, nullptr };
      ID3D11UnorderedAccessView *null_uavs[2] = { nullptr, nullptr };
      device_ctx->CSSetShaderResources(0, 2, null_srvs);
      device_ctx->CSSetUnorderedAccessViews(0, 2, null_uavs, nullptr);

      // ===== Pass 2: Global reduction =====
      device_ctx->CSSetShader(hdr_pass2_cs.get(), nullptr, 0);
      ID3D11ShaderResourceView *group_srv = hdr_group_results_srv.get();
      device_ctx->CSSetShaderResources(0, 1, &group_srv);
      ID3D11UnorderedAccessView *pass2_uavs[] = { hdr_final_result_uav.get(), hdr_global_histogram_uav.get() };
      device_ctx->CSSetUnorderedAccessViews(0, 2, pass2_uavs, nullptr);
      ID3D11Buffer *cbuf = hdr_reduce_cbuf.get();
      device_ctx->CSSetConstantBuffers(0, 1, &cbuf);

      device_ctx->Dispatch(1, 1, 1);  // Single group of 256 threads

      // Unbind all CS resources
      device_ctx->CSSetShaderResources(0, 1, &null_srv);
      device_ctx->CSSetUnorderedAccessViews(0, 2, null_uavs, nullptr);
      ID3D11Buffer *null_cb = nullptr;
      device_ctx->CSSetConstantBuffers(0, 1, &null_cb);
      device_ctx->CSSetShader(nullptr, nullptr, 0);

      // Copy final result to staging buffer for CPU readback next frame
      device_ctx->CopyResource(hdr_staging_buf.get(), hdr_final_result_buf.get());

      hdr_analysis_pending = true;
    }

    /**
     * @brief Read HDR analysis results from the staging buffer (previous frame).
     * GPU has already reduced all groups to one FinalResult — CPU just reads it
     * and computes PQ-domain percentiles from the histogram.
     */
    void
    read_hdr_analysis_results() {
      D3D11_MAPPED_SUBRESOURCE mapped = {};
      HRESULT status = device_ctx->Map(hdr_staging_buf.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);

      if (status == DXGI_ERROR_WAS_STILL_DRAWING) {
        // GPU hasn't finished yet — skip this readback, try next frame
        return;
      }

      if (FAILED(status)) {
        BOOST_LOG(debug) << "HDR staging Map failed: " << util::log_hex(status);
        return;
      }

      auto *result = reinterpret_cast<const FinalResult *>(mapped.pData);

      if (result->pixelCount > 0) {
        hdr_luminance_stats_out.min_maxrgb = result->minMaxRGB;
        hdr_luminance_stats_out.max_maxrgb = result->maxMaxRGB;
        hdr_luminance_stats_out.avg_maxrgb = result->sumMaxRGB / static_cast<float>(result->pixelCount);
        // HDR Vivid's average is a PQ-domain statistic like the variance beside it, so
        // the GPU accumulates PQ(maxRGB) per pixel and this divides that sum. It cannot
        // be derived from avg_maxrgb above (PQ is concave, so PQ(mean) is far above
        // mean(PQ) on a dark frame with highlights) nor from the histogram below (that
        // is populated from one representative sample per analysis cell, which is a
        // distribution to take percentiles from, not an exact mean).
        //
        // std::clamp() alone would launder bad data: it passes a NaN straight through,
        // and it turns an implausible value into exactly 1.0, which reads downstream as
        // a legitimate "entire frame at 10,000 nits". So screen first and clamp only the
        // FP32 rounding overshoot a sum of in-range summands can produce. An implausible
        // value stays zero, which vivid_from_stats() reads as "the analyzer produced no
        // PQ average" and withholds Vivid on. The rest of the sample is still published:
        // HDR10+ does not read this field and carries its own statistics.
        const float mean_pq = result->sumMaxRGB_PQ / static_cast<float>(result->pixelCount);
        hdr_luminance_stats_out.avg_maxrgb_pq =
          (std::isfinite(mean_pq) && mean_pq >= -0.001f && mean_pq <= 1.001f) ?
            std::clamp(mean_pq, 0.0f, 1.0f) :
            0.0f;

        // HDR Vivid defines variance as P90-P10 in normalized PQ signal space.
        // Retain P99 in nits for the independent HDR10+ path, and fill the nine
        // percentiles ST 2094-40 deployment profiles carry from the same walk.
        const uint32_t total = result->pixelCount;
        const auto &percentages = ::video::hdr_metadata::hdr10plus_percentages;
        constexpr size_t kDistCount = percentages.size();

        std::array<uint32_t, kDistCount> dist_targets {};
        std::array<bool, kDistCount> dist_found {};
        for (size_t p = 0; p < kDistCount; ++p) {
          dist_targets[p] = static_cast<uint32_t>(std::ceil(total * (percentages[p] / 100.0f)));
        }
        const uint32_t target_10 = static_cast<uint32_t>(std::ceil(total * 0.10f));
        const uint32_t target_90 = static_cast<uint32_t>(std::ceil(total * 0.90f));
        const uint32_t target_99 = static_cast<uint32_t>(std::ceil(total * 0.99f));
        uint32_t cumulative = 0;
        bool found_10 = false;
        bool found_90 = false;
        bool found_99 = false;

        for (uint32_t i = 0; i < HISTOGRAM_BINS; i++) {
          cumulative += result->histogram[i];
          const float pq_bin_center = (static_cast<float>(i) + 0.5f) / HISTOGRAM_BINS;
          for (size_t p = 0; p < kDistCount; ++p) {
            if (!dist_found[p] && cumulative >= dist_targets[p]) {
              hdr_luminance_stats_out.distribution_maxrgb[p] =
                ::video::hdr_metadata::pq_to_nits(pq_bin_center);
              dist_found[p] = true;
            }
          }
          if (!found_10 && cumulative >= target_10) {
            hdr_luminance_stats_out.percentile_10_pq = pq_bin_center;
            found_10 = true;
          }
          if (!found_90 && cumulative >= target_90) {
            hdr_luminance_stats_out.percentile_90_pq = pq_bin_center;
            found_90 = true;
          }
          if (!found_99 && cumulative >= target_99) {
            hdr_luminance_stats_out.percentile_99 =
              ::video::hdr_metadata::pq_to_nits(pq_bin_center);
            found_99 = true;
          }
          // The 99th percentile is the last of every target set, so the walk can stop
          // once both it and the distribution are filled.
          if (found_99 && dist_found[kDistCount - 1]) {
            break;
          }
        }

        hdr_luminance_stats_out.analysis_max_nits = hdr_analysis_max_nits;
        hdr_luminance_stats_out.sample_sequence = ++hdr_analysis_sample_sequence;
        hdr_luminance_stats_out.valid = true;
      }

      device_ctx->Unmap(hdr_staging_buf.get(), 0);
      hdr_analysis_pending = false;
    }

    // ===== Compute-shader RGB->P010 fast path (Phase 1) =====
    // Allocates a P010 scratch texture with UAV bind, creates plane UAVs and a layout cbuffer.
    // Probes runtime support; if anything fails, leaves cs_path_active = false (PS path stays active).
    // Called from init_output() after standard setup. Non-fatal on failure.
    void
    init_compute_path(int out_width, int out_height,
                      int active_w, int active_h,
                      int active_offset_x, int active_offset_y,
                      const ::video::sunshine_colorspace_t &colorspace) {
      cs_path_active = false;
      cs_writes_output_directly = false;
      cs_for_p010 = false;
      cs_is_scaled = false;
      cs_p010.reset();
      cs_p010_hdr_analysis.reset();
      cs_nv12_pass.reset();
      cs_nv12_linear.reset();
      cs_p010_scaled.reset();
      cs_p010_scaled_hdr_analysis.reset();
      cs_nv12_pass_scaled.reset();
      cs_nv12_linear_scaled.reset();
      hdr_analysis_snapshot_tex.reset();
      hdr_analysis_snapshot_srv.reset();
      hdr_analysis_snapshot_uav.reset();
      hdr_analysis_pq_tex.reset();
      hdr_analysis_pq_srv.reset();
      hdr_analysis_pq_uav.reset();
      hdr_analysis_snapshot_cbuf.reset();
      hdr_analysis_snapshot_enabled = false;
      cs_scratch_tex.reset();
      cs_y_uav.reset();
      cs_uv_uav.reset();
      cs_layout_cbuf.reset();
      cs_fallback_reason.clear();

      // Phase 1/2: only NV12 (SDR) or P010 (HDR) supported.
      const bool is_p010 = (format == DXGI_FORMAT_P010);
      const bool is_nv12 = (format == DXGI_FORMAT_NV12);
      if (!is_p010 && !is_nv12) {
        cs_fallback_reason = "unsupported_format";
        return;
      }

      // For HDR P010 we require PQ or HLG colorspace (linear-light source).
      const bool use_pq = ::video::colorspace_is_pq(colorspace);
      const bool use_hlg = ::video::colorspace_is_hlg(colorspace);
      if (is_p010 && !use_pq && !use_hlg) {
        cs_fallback_reason = "unsupported_colorspace";
        return;
      }
      // For SDR NV12 we require a non-PQ/HLG colorspace.
      if (is_nv12 && (use_pq || use_hlg)) {
        cs_fallback_reason = "unsupported_colorspace";
        return;
      }

      // Phase 2B: scaling supported via *_scaled variants. Rotation still TBD.
      const bool is_scaled = (active_w != display->width || active_h != display->height);
      if (display->display_rotation != DXGI_MODE_ROTATION_UNSPECIFIED &&
          display->display_rotation != DXGI_MODE_ROTATION_IDENTITY) {
        cs_fallback_reason = "rotation";
        return;
      }

      // Automatic mode uses the compute path where it has a clear payoff:
      // scaling, or fusing HDR conversion with luminance-analysis sampling.
      const auto &cfg = config::video.capture_compute_shader;
      if (cfg == "off") {
        cs_fallback_reason = "disabled";
        return;
      }
      if (cfg == "auto" && !is_scaled && !(is_p010 && hdr_analysis_enabled)) {
        cs_fallback_reason = "not_beneficial";
        return;
      }

      // Output dimensions must be even (4:2:0 sub-sampling) and aligned for plane UAV.
      if ((out_width & 1) != 0 || (out_height & 1) != 0 ||
          (active_offset_x & 1) != 0 || (active_offset_y & 1) != 0) {
        cs_fallback_reason = "unaligned_output";
        return;
      }

      // Compile-time blobs present?
      if (is_p010) {
        auto &blob = is_scaled
                       ? (use_pq ? convert_yuv420_p010_cs_perceptual_quantizer_scaled_hlsl
                                 : convert_yuv420_p010_cs_hybrid_log_gamma_scaled_hlsl)
                       : (use_pq ? convert_yuv420_p010_cs_perceptual_quantizer_hlsl
                                 : convert_yuv420_p010_cs_hybrid_log_gamma_hlsl);
        if (!blob) {
          cs_fallback_reason = "shader_unavailable";
          BOOST_LOG(info) << "CS path skipped: P010 compute shader blob unavailable";
          return;
        }
      } else {
        // SDR NV12: need at least one of passthrough/linear (for the right scale mode) to be useful.
        const bool any_blob = is_scaled
                                ? (convert_yuv420_nv12_cs_passthrough_scaled_hlsl ||
                                   convert_yuv420_nv12_cs_linear_scaled_hlsl)
                                : (convert_yuv420_nv12_cs_passthrough_hlsl ||
                                   convert_yuv420_nv12_cs_linear_hlsl);
        if (!any_blob) {
          cs_fallback_reason = "shader_unavailable";
          BOOST_LOG(info) << "CS path skipped: NV12 compute shader blobs unavailable";
          return;
        }
      }

      // --- Probe device support: typed UAV stores for plane formats ---
      auto has_uav_typed_store = [&](DXGI_FORMAT fmt) -> bool {
        D3D11_FEATURE_DATA_FORMAT_SUPPORT2 fs = { fmt };
        if (FAILED(device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &fs, sizeof(fs)))) return false;
        return (fs.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
      };
      const DXGI_FORMAT y_fmt = is_p010 ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
      const DXGI_FORMAT uv_fmt = is_p010 ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;
      if (!has_uav_typed_store(y_fmt) || !has_uav_typed_store(uv_fmt)) {
        cs_fallback_reason = "device_capability";
        BOOST_LOG(info) << "CS path skipped: device lacks typed UAV store for plane formats";
        return;
      }

      // The output format itself must support typed UAVs (for direct or scratch).
      D3D11_FEATURE_DATA_FORMAT_SUPPORT fs_yuv = { format };
      if (FAILED(device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT, &fs_yuv, sizeof(fs_yuv))) ||
          !(fs_yuv.OutFormatSupport & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW)) {
        cs_fallback_reason = "device_capability";
        BOOST_LOG(info) << "CS path skipped: device lacks typed UAV support for output format";
        return;
      }

      // --- Try to bind UAVs directly on output_texture first (fast path).
      //     If output_texture wasn't created with BIND_UNORDERED_ACCESS (typical for
      //     ffmpeg/NVENC/AMF input pools), CreateUnorderedAccessView returns E_INVALIDARG
      //     and we fall back to a scratch + CopyResource.
      D3D11_UNORDERED_ACCESS_VIEW_DESC y_uav_desc = {};
      y_uav_desc.Format = y_fmt;
      y_uav_desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;

      D3D11_UNORDERED_ACCESS_VIEW_DESC uv_uav_desc = {};
      uv_uav_desc.Format = uv_fmt;
      uv_uav_desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;

      bool direct_ok = false;
      if (output_texture) {
        uav_t y_uav_try, uv_uav_try;
        auto s1 = device->CreateUnorderedAccessView(output_texture.get(), &y_uav_desc, &y_uav_try);
        auto s2 = SUCCEEDED(s1)
                    ? device->CreateUnorderedAccessView(output_texture.get(), &uv_uav_desc, &uv_uav_try)
                    : s1;
        if (SUCCEEDED(s1) && SUCCEEDED(s2)) {
          cs_y_uav = std::move(y_uav_try);
          cs_uv_uav = std::move(uv_uav_try);
          cs_writes_output_directly = true;
          direct_ok = true;
        }
      }

      // --- Fall back to scratch (same format as output) with UAV when direct binding isn't possible.
      if (!direct_ok) {
        D3D11_TEXTURE2D_DESC scratch_desc = {};
        scratch_desc.Width = out_width;
        scratch_desc.Height = out_height;
        scratch_desc.MipLevels = 1;
        scratch_desc.ArraySize = 1;
        scratch_desc.SampleDesc.Count = 1;
        scratch_desc.Format = format;
        scratch_desc.Usage = D3D11_USAGE_DEFAULT;
        scratch_desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        auto status = device->CreateTexture2D(&scratch_desc, nullptr, &cs_scratch_tex);
        if (FAILED(status)) {
          cs_fallback_reason = "resource_creation";
          BOOST_LOG(info) << "CS path skipped: failed to create scratch with UAV bind: "
                          << util::log_hex(status);
          return;
        }

        status = device->CreateUnorderedAccessView(cs_scratch_tex.get(), &y_uav_desc, &cs_y_uav);
        if (FAILED(status)) {
          cs_fallback_reason = "resource_creation";
          BOOST_LOG(info) << "CS path skipped: failed to create Y-plane UAV: " << util::log_hex(status);
          cs_scratch_tex.reset();
          return;
        }

        status = device->CreateUnorderedAccessView(cs_scratch_tex.get(), &uv_uav_desc, &cs_uv_uav);
        if (FAILED(status)) {
          cs_fallback_reason = "resource_creation";
          BOOST_LOG(info) << "CS path skipped: failed to create UV-plane UAV: " << util::log_hex(status);
          cs_scratch_tex.reset();
          cs_y_uav.reset();
          return;
        }
      }

      // --- One-time clear: ensure letterbox/aspect-padding pixels are black (Y=0)
      //     and chroma neutral (UV=0.5). CS only writes inside the active rect
      //     to save dispatch work; matches PS path's one-shot ClearRenderTargetView.
      {
        const float y_black[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        const float uv_neutral[4] = { 0.5f, 0.5f, 0.5f, 0.5f };
        device_ctx->ClearUnorderedAccessViewFloat(cs_y_uav.get(), y_black);
        device_ctx->ClearUnorderedAccessViewFloat(cs_uv_uav.get(), uv_neutral);
      }

      // --- Create CS shader(s) ---
      auto make_cs = [&](ID3DBlob *blob, cs_t &out) -> bool {
        if (!blob) return false;
        HRESULT s = device->CreateComputeShader(
          blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &out);
        if (FAILED(s)) {
          BOOST_LOG(info) << "CS path: CreateComputeShader failed: " << util::log_hex(s);
          out.reset();
          return false;
        }
        return true;
      };

      bool any_cs_created = false;
      if (is_p010) {
        if (is_scaled) {
          auto &blob = use_pq ? convert_yuv420_p010_cs_perceptual_quantizer_scaled_hlsl
                              : convert_yuv420_p010_cs_hybrid_log_gamma_scaled_hlsl;
          any_cs_created = make_cs(blob.get(), cs_p010_scaled);
          if (hdr_analysis_enabled) {
            auto &analysis_blob = use_pq ?
                                    convert_yuv420_p010_cs_perceptual_quantizer_scaled_hdr_analysis_hlsl :
                                    convert_yuv420_p010_cs_hybrid_log_gamma_scaled_hdr_analysis_hlsl;
            make_cs(analysis_blob.get(), cs_p010_scaled_hdr_analysis);
          }
        } else {
          auto &blob = use_pq ? convert_yuv420_p010_cs_perceptual_quantizer_hlsl
                              : convert_yuv420_p010_cs_hybrid_log_gamma_hlsl;
          any_cs_created = make_cs(blob.get(), cs_p010);
          if (hdr_analysis_enabled) {
            auto &analysis_blob = use_pq ?
                                    convert_yuv420_p010_cs_perceptual_quantizer_hdr_analysis_hlsl :
                                    convert_yuv420_p010_cs_hybrid_log_gamma_hdr_analysis_hlsl;
            make_cs(analysis_blob.get(), cs_p010_hdr_analysis);
          }
        }
      } else {
        if (is_scaled) {
          bool a = make_cs(convert_yuv420_nv12_cs_passthrough_scaled_hlsl.get(), cs_nv12_pass_scaled);
          bool b = make_cs(convert_yuv420_nv12_cs_linear_scaled_hlsl.get(), cs_nv12_linear_scaled);
          any_cs_created = a || b;
        } else {
          bool a = make_cs(convert_yuv420_nv12_cs_passthrough_hlsl.get(), cs_nv12_pass);
          bool b = make_cs(convert_yuv420_nv12_cs_linear_hlsl.get(), cs_nv12_linear);
          any_cs_created = a || b;
        }
      }
      if (!any_cs_created) {
        cs_fallback_reason = "shader_creation";
        cs_scratch_tex.reset();
        cs_y_uav.reset();
        cs_uv_uav.reset();
        return;
      }

      // --- Layout cbuffer ---
      struct LayoutCB {
        int32_t out_rect_offset[2];
        int32_t out_rect_size[2];
        int32_t src_size[2];
        int32_t pad[2];
      } layout = {
        { active_offset_x, active_offset_y },
        { active_w, active_h },
        { display->width, display->height },
        { 0, 0 },
      };
      cs_layout_cbuf = make_buffer(device.get(), layout);
      if (!cs_layout_cbuf) {
        cs_fallback_reason = "resource_creation";
        BOOST_LOG(info) << "CS path skipped: failed to create layout cbuffer";
        cs_p010.reset();
        cs_p010_hdr_analysis.reset();
        cs_nv12_pass.reset();
        cs_nv12_linear.reset();
        cs_p010_scaled.reset();
        cs_p010_scaled_hdr_analysis.reset();
        cs_nv12_pass_scaled.reset();
        cs_nv12_linear_scaled.reset();
        cs_scratch_tex.reset();
        cs_y_uav.reset();
        cs_uv_uav.reset();
        return;
      }

      // Let the HDR converter write capped per-cell min/max/average statistics while
      // it already owns the shared scRGB source. The luminance passes consume them
      // after the keyed mutex is released, avoiding a full-resolution CopyResource
      // without losing extrema to a point-sampled analysis grid.
      const bool has_hdr_analysis_shader =
        is_p010 && (is_scaled ? bool(cs_p010_scaled_hdr_analysis) : bool(cs_p010_hdr_analysis));
      if (hdr_analysis_enabled && has_hdr_analysis_shader &&
          active_w >= static_cast<int>(hdr_analysis_width) &&
          active_h >= static_cast<int>(hdr_analysis_height)) {
        D3D11_TEXTURE2D_DESC snapshot_desc = {};
        snapshot_desc.Width = hdr_analysis_width;
        snapshot_desc.Height = hdr_analysis_height;
        snapshot_desc.MipLevels = 1;
        snapshot_desc.ArraySize = 1;
        snapshot_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        snapshot_desc.SampleDesc.Count = 1;
        snapshot_desc.Usage = D3D11_USAGE_DEFAULT;
        snapshot_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

        auto snapshot_status =
          device->CreateTexture2D(&snapshot_desc, nullptr, &hdr_analysis_snapshot_tex);
        if (SUCCEEDED(snapshot_status)) {
          snapshot_status = device->CreateShaderResourceView(
            hdr_analysis_snapshot_tex.get(), nullptr, &hdr_analysis_snapshot_srv);
        }
        if (SUCCEEDED(snapshot_status)) {
          snapshot_status = device->CreateUnorderedAccessView(
            hdr_analysis_snapshot_tex.get(), nullptr, &hdr_analysis_snapshot_uav);
        }

        // A fifth per-cell scalar: the average PQ-coded maxRGB HDR Vivid reports.
        // Single channel, and a normalized PQ signal quantizes into FP16 with room to
        // spare, so this adds a quarter of the snapshot's footprint.
        D3D11_TEXTURE2D_DESC pq_desc = snapshot_desc;
        pq_desc.Format = DXGI_FORMAT_R16_FLOAT;
        if (SUCCEEDED(snapshot_status)) {
          snapshot_status = device->CreateTexture2D(&pq_desc, nullptr, &hdr_analysis_pq_tex);
        }
        if (SUCCEEDED(snapshot_status)) {
          snapshot_status = device->CreateShaderResourceView(
            hdr_analysis_pq_tex.get(), nullptr, &hdr_analysis_pq_srv);
        }
        if (SUCCEEDED(snapshot_status)) {
          snapshot_status = device->CreateUnorderedAccessView(
            hdr_analysis_pq_tex.get(), nullptr, &hdr_analysis_pq_uav);
        }

        AnalysisParams snapshot_layout = {
          hdr_analysis_width,
          hdr_analysis_height,
          static_cast<uint32_t>(active_w),
          static_cast<uint32_t>(active_h),
          1,
          hdr_analysis_max_nits,
          {},
        };
        if (SUCCEEDED(snapshot_status)) {
          hdr_analysis_snapshot_cbuf = make_buffer(device.get(), snapshot_layout);
          if (!hdr_analysis_snapshot_cbuf) {
            snapshot_status = E_FAIL;
          }
        }

        if (SUCCEEDED(snapshot_status)) {
          hdr_analysis_snapshot_enabled = true;
          BOOST_LOG(info) << "HDR analysis cell statistics fused into P010 conversion at "
                          << hdr_analysis_width << "x" << hdr_analysis_height;
        } else {
          hdr_analysis_snapshot_tex.reset();
          hdr_analysis_snapshot_srv.reset();
          hdr_analysis_snapshot_uav.reset();
          hdr_analysis_pq_tex.reset();
          hdr_analysis_pq_srv.reset();
          hdr_analysis_pq_uav.reset();
          hdr_analysis_snapshot_cbuf.reset();
          BOOST_LOG(info) << "HDR analysis snapshot unavailable, full-resolution copy fallback active: "
                          << util::log_hex(snapshot_status);
        }
      }

      // Dispatch only over the active rect (saves work on letterbox/pillarbox borders;
      // those were initialized to black by the one-time clear above).
      cs_dispatch_groups_x = (active_w + 15) / 16;
      cs_dispatch_groups_y = (active_h + 15) / 16;

      // Logical output extent the encoder consumes. Intel QSV input surfaces are
      // commonly aligned up internally (e.g. 1080 -> 1088 rows), so the underlying
      // ID3D11Texture2D backing output_texture can be larger than out_width/out_height.
      // We use CopySubresourceRegion with this explicit box on the scratch path so
      // dst/src dimensions never have to match the resource desc exactly.
      cs_copy_w = out_width;
      cs_copy_h = out_height;

      cs_path_active = true;
      cs_fallback_reason.clear();
      cs_use_pq = use_pq;
      cs_for_p010 = is_p010;
      cs_is_scaled = is_scaled;
      if (is_p010) {
        BOOST_LOG(info) << "CS RGB->P010 fast path enabled ("
                        << (use_pq ? "PQ" : "HLG") << ", "
                        << (is_scaled ? "scaled, " : "")
                        << display->width << "x" << display->height << " -> "
                        << active_w << "x" << active_h
                        << " into " << out_width << "x" << out_height
                        << (cs_writes_output_directly ? ", direct UAV" : ", scratch+CopyResource")
                        << ")";
      } else {
        const bool has_pass = is_scaled ? bool(cs_nv12_pass_scaled) : bool(cs_nv12_pass);
        const bool has_lin = is_scaled ? bool(cs_nv12_linear_scaled) : bool(cs_nv12_linear);
        BOOST_LOG(info) << "CS RGB->NV12 fast path enabled (SDR"
                        << (is_scaled ? ", scaled" : "")
                        << (has_pass ? ", passthrough" : "")
                        << (has_lin ? ", linear" : "")
                        << ", " << display->width << "x" << display->height << " -> "
                        << active_w << "x" << active_h
                        << " into " << out_width << "x" << out_height
                        << (cs_writes_output_directly ? ", direct UAV" : ", scratch+CopyResource")
                        << ")";
      }
    }

    // Dispatch the CS converter. Writes directly into output_texture's UAVs when
    // the device allowed it, otherwise into a scratch then CopyResource.
    // Caller must already hold the encoder mutex on `input_srv`.
    bool
    try_dispatch_cs_convert(
      ID3D11ShaderResourceView *input_srv,
      cs_t &shader,
      bool write_hdr_analysis_snapshot,
      gpu_timing_sample_t *timing) {
      if (!cs_path_active) return false;
      if (!shader) return false;
      if (timing) {
        timing->cs_used = true;
        timing->scratch_copy = !cs_writes_output_directly;
        timing->direct_uav = cs_writes_output_directly;
        timing->p010 = cs_for_p010;
        timing->scaled = cs_is_scaled;
      }

      // Unbind PS-side render targets to avoid SRV/RTV hazards.
      ID3D11RenderTargetView *null_rtv[2] = { nullptr, nullptr };
      device_ctx->OMSetRenderTargets(2, null_rtv, nullptr);

      // Bind CS resources. Sampler is bound for the *_scaled variants which
      // call SampleLevel; the no-scale variants only Load() and ignore it
      // (binding is cheap and avoids per-dispatch branches).
      device_ctx->CSSetShader(shader.get(), nullptr, 0);
      device_ctx->CSSetShaderResources(0, 1, &input_srv);
      ID3D11SamplerState *cs_samp = sampler_linear.get();
      device_ctx->CSSetSamplers(0, 1, &cs_samp);
      ID3D11UnorderedAccessView *uavs[4] = {
        cs_y_uav.get(),
        cs_uv_uav.get(),
        write_hdr_analysis_snapshot ? hdr_analysis_snapshot_uav.get() : nullptr,
        write_hdr_analysis_snapshot ? hdr_analysis_pq_uav.get() : nullptr,
      };
      const UINT uav_count = write_hdr_analysis_snapshot ? 4 : 2;
      device_ctx->CSSetUnorderedAccessViews(0, uav_count, uavs, nullptr);
      ID3D11Buffer *cbufs[3] = {
        color_matrix.get(),
        cs_layout_cbuf.get(),
        write_hdr_analysis_snapshot ? hdr_analysis_snapshot_cbuf.get() : nullptr,
      };
      const UINT cbuf_count = write_hdr_analysis_snapshot ? 3 : 2;
      device_ctx->CSSetConstantBuffers(0, cbuf_count, cbufs);
      if (hlg_display_cbuf) {
        ID3D11Buffer *hlg_cbuf = hlg_display_cbuf.get();
        device_ctx->CSSetConstantBuffers(3, 1, &hlg_cbuf);
      }

      // Dispatch covers only the active rect (precomputed in init_compute_path).
      device_ctx->Dispatch((UINT) cs_dispatch_groups_x, (UINT) cs_dispatch_groups_y, 1);
      if (timing) {
        device_ctx->End(timing->after_dispatch.get());
      }

      // Unbind CS resources to release the UAVs before any subsequent ops.
      ID3D11ShaderResourceView *null_srv = nullptr;
      ID3D11UnorderedAccessView *null_uavs[4] = { nullptr, nullptr, nullptr, nullptr };
      ID3D11SamplerState *null_samp = nullptr;
      device_ctx->CSSetShaderResources(0, 1, &null_srv);
      device_ctx->CSSetSamplers(0, 1, &null_samp);
      device_ctx->CSSetUnorderedAccessViews(0, uav_count, null_uavs, nullptr);
      ID3D11Buffer *null_cb[3] = { nullptr, nullptr, nullptr };
      device_ctx->CSSetConstantBuffers(0, cbuf_count, null_cb);
      ID3D11Buffer *null_hlg_cbuf = nullptr;
      device_ctx->CSSetConstantBuffers(3, 1, &null_hlg_cbuf);
      device_ctx->CSSetShader(nullptr, nullptr, 0);
      if (timing) {
        device_ctx->End(timing->before_copy.get());
      }

      // Only copy when we couldn't bind the UAV directly to output_texture.
      // Use CopySubresourceRegion with an explicit box so a padded dst (e.g. Intel
      // QSV's internally row-aligned NV12/P010 surface) still receives the correct
      // active region. Plain CopyResource silently fails when src/dst desc differ.
      if (!cs_writes_output_directly) {
        D3D11_BOX src_box = { 0, 0, 0, (UINT) cs_copy_w, (UINT) cs_copy_h, 1 };
        device_ctx->CopySubresourceRegion(output_texture.get(), 0, 0, 0, 0,
                                          cs_scratch_tex.get(), 0, &src_box);
      }
      if (timing) {
        device_ctx->End(timing->after_copy.get());
      }
      return true;
    }

    // Intermediate storage for luminance stats (written by readback, consumed by convert caller)
    platf::hdr_frame_luminance_stats_t hdr_luminance_stats_out;
  };

  class d3d_avcodec_encode_device_t: public avcodec_encode_device_t {
  public:
    int
    init(std::shared_ptr<platf::display_t> display, adapter_t::pointer adapter_p, pix_fmt_e pix_fmt) {
      // Encoders reached through avcodec never emit HDR Vivid: FFmpeg has no
      // encoder-side serializer for AV_FRAME_DATA_DYNAMIC_HDR_VIVID, so the side
      // data is attached and dropped. HDR10+ does get written out, so it stays.
      int result = base.init(display, adapter_p, pix_fmt, { .hdr10plus = true, .vivid = false });
      data = base.device.get();
      return result;
    }

    int
    convert(platf::img_t &img_base) override {
      int result = base.convert(img_base);
      // Propagate per-frame luminance stats from GPU analyzer to encode device
      hdr_luminance_stats = base.hdr_luminance_stats_out;
      return result;
    }

    void
    apply_colorspace() override {
      base.apply_colorspace(colorspace);
    }

    void
    init_hwframes(AVHWFramesContext *frames) override {
      // We may be called with a QSV or D3D11VA context
      if (frames->device_ctx->type == AV_HWDEVICE_TYPE_D3D11VA) {
        auto d3d11_frames = (AVD3D11VAFramesContext *) frames->hwctx;

        // The encoder requires textures with D3D11_BIND_RENDER_TARGET set
        d3d11_frames->BindFlags = D3D11_BIND_RENDER_TARGET;
        d3d11_frames->MiscFlags = 0;
      }

      // We require a single texture
      frames->initial_pool_size = 1;
    }

    int
    prepare_to_derive_context(int hw_device_type) override {
      // QuickSync requires our device to be multithread-protected
      if (hw_device_type == AV_HWDEVICE_TYPE_QSV) {
        multithread_t mt;

        auto status = base.device->QueryInterface(IID_ID3D11Multithread, (void **) &mt);
        if (FAILED(status)) {
          BOOST_LOG(warning) << "Failed to query ID3D11Multithread interface from device [0x"sv << util::hex(status).to_string_view() << ']';
          return -1;
        }

        mt->SetMultithreadProtected(TRUE);
      }

      return 0;
    }

    int
    set_frame(AVFrame *frame, AVBufferRef *hw_frames_ctx) override {
      this->hwframe.reset(frame);
      this->frame = frame;

      // Populate this frame with a hardware buffer if one isn't there already
      if (!frame->buf[0]) {
        auto err = av_hwframe_get_buffer(hw_frames_ctx, frame, 0);
        if (err) {
          char err_str[AV_ERROR_MAX_STRING_SIZE] { 0 };
          BOOST_LOG(error) << "Failed to get hwframe buffer: "sv << av_make_error_string(err_str, AV_ERROR_MAX_STRING_SIZE, err);
          return -1;
        }
      }

      // If this is a frame from a derived context, we'll need to map it to D3D11
      ID3D11Texture2D *frame_texture;
      if (frame->format != AV_PIX_FMT_D3D11) {
        frame_t d3d11_frame { av_frame_alloc() };

        d3d11_frame->format = AV_PIX_FMT_D3D11;

        auto err = av_hwframe_map(d3d11_frame.get(), frame, AV_HWFRAME_MAP_WRITE | AV_HWFRAME_MAP_OVERWRITE);
        if (err) {
          char err_str[AV_ERROR_MAX_STRING_SIZE] { 0 };
          BOOST_LOG(error) << "Failed to map D3D11 frame: "sv << av_make_error_string(err_str, AV_ERROR_MAX_STRING_SIZE, err);
          return -1;
        }

        // Get the texture from the mapped frame
        frame_texture = (ID3D11Texture2D *) d3d11_frame->data[0];
      }
      else {
        // Otherwise, we can just use the texture inside the original frame
        frame_texture = (ID3D11Texture2D *) frame->data[0];
      }

      // No client config in scope this deep in the frame-pool setup, so the codec
      // comes from the member video.cpp filled in when this device was created.
      return base.init_output(frame_texture, frame->width, frame->height, colorspace, video_format);
    }

  private:
    d3d_base_encode_device base;
    frame_t hwframe;
  };

  class d3d_nvenc_encode_device_t: public nvenc_encode_device_t {
  public:
    bool
    init_device(std::shared_ptr<platf::display_t> display, adapter_t::pointer adapter_p, pix_fmt_e pix_fmt) {
      // The native NVENC path hand-writes both T.35 payloads (nvenc_base.cpp).
      if (base.init(display, adapter_p, pix_fmt, { .hdr10plus = true, .vivid = true })) return false;

      auto factory = nvenc::nvenc_dynamic_factory::get();
      if (!factory) return false;

      if (pix_fmt == pix_fmt_e::yuv444p16) {
        nvenc_d3d = factory->create_nvenc_d3d11_on_cuda(base.device.get());
      }
      else {
        nvenc_d3d = factory->create_nvenc_d3d11_native(base.device.get());
      }

      if (!nvenc_d3d) return false;

      buffer_format = pix_fmt;
      nvenc = nvenc_d3d.get();

      return true;
    }

    bool
    init_encoder(const ::video::config_t &client_config, const ::video::sunshine_colorspace_t &colorspace, bool is_probe = false) override {
      if (!nvenc_d3d) return false;

      hdr_luminance_analysis_available = false;
      if (!nvenc_d3d->create_encoder(config::video.nv, client_config, colorspace, buffer_format)) return false;

      base.apply_colorspace(colorspace);
      base.set_client_sdr_white(client_config.hdr_capabilities.sdr_white_nits);
      if (base.init_output(nvenc_d3d->get_input_texture(), client_config.width, client_config.height, colorspace, client_config.videoFormat, is_probe)) {
        return false;
      }

      hdr_luminance_analysis_available = base.hdr_luminance_analysis_available();
      return true;
    }

    int
    convert(platf::img_t &img_base) override {
      int result = base.convert(img_base);
      // Propagate per-frame luminance stats from GPU analyzer to encode device
      hdr_luminance_stats = base.hdr_luminance_stats_out;
      return result;
    }

    void
    set_client_sdr_white_nits(float nits) override {
      base.set_client_sdr_white(nits);
    }

  private:
    d3d_base_encode_device base;
    std::unique_ptr<nvenc::nvenc_d3d11> nvenc_d3d;
    platf::pix_fmt_e buffer_format = platf::pix_fmt_e::unknown;
  };

  class d3d_amf_encode_device_t: public amf_encode_device_t {
  public:
    bool
    init_device(std::shared_ptr<platf::display_t> display, adapter_t::pointer adapter_p, pix_fmt_e pix_fmt) {
      // The AMF path splices HDR10+ / HDR Vivid into the bitstream itself (#939), so
      // the luminance analyzer that feeds it has to be switched on here. This was off
      // while AMF could only carry static metadata, and running the analyzer then
      // would have burned GPU time for nothing.
      if (base.init(display, adapter_p, pix_fmt, { .hdr10plus = true, .vivid = true })) return false;

      amf_d3d = ::amf::create_amf_d3d11(base.device.get());
      if (!amf_d3d) return false;

      buffer_format = pix_fmt;
      amf = amf_d3d.get();

      return true;
    }

    bool
    init_encoder(const ::video::config_t &client_config, const ::video::sunshine_colorspace_t &colorspace, bool is_probe = false) override {
      if (!amf_d3d) return false;

      ::amf::amf_config amf_cfg;
      amf_cfg.avcodec_compat = config::video.amd.amd_avcodec_compat;

      // Pass AMF SDK integer values directly from config
      if (client_config.videoFormat == 0) {
        amf_cfg.usage = config::video.amd.amd_usage_h264;
        amf_cfg.quality_preset = config::video.amd.amd_quality_h264;
        amf_cfg.rc_mode = config::video.amd.amd_rc_h264;
        if (is_quality_vbr_rate_control(amf_cfg.rc_mode)) {
          amf_cfg.qvbr_quality_level = config::video.amd.amd_qvbr_quality;
        }
      }
      else if (client_config.videoFormat == 1) {
        amf_cfg.usage = config::video.amd.amd_usage_hevc;
        amf_cfg.quality_preset = config::video.amd.amd_quality_hevc;
        amf_cfg.rc_mode = config::video.amd.amd_rc_hevc;
        if (is_quality_vbr_rate_control(amf_cfg.rc_mode)) {
          amf_cfg.qvbr_quality_level = config::video.amd.amd_qvbr_quality;
        }
      }
      else {
        amf_cfg.usage = config::video.amd.amd_usage_av1;
        amf_cfg.quality_preset = config::video.amd.amd_quality_av1;
        amf_cfg.rc_mode = config::video.amd.amd_rc_av1;
        if (is_quality_vbr_rate_control(amf_cfg.rc_mode)) {
          amf_cfg.qvbr_quality_level = config::video.amd.amd_qvbr_quality;
        }
      }

      amf_cfg.preanalysis = config::video.amd.amd_preanalysis;
      amf_cfg.vbaq = config::video.amd.amd_vbaq;
      amf_cfg.enforce_hrd = config::video.amd.amd_enforce_hrd;
      amf_cfg.h264_cabac = (config::video.amd.amd_coder != 2);  // 2 = CAVLC
      if (config::video.amd.amd_coder != 0) {  // 0 = auto / AMF_VIDEO_ENCODER_UNDEFINED
        amf_cfg.h264_coding_mode = config::video.amd.amd_coder;
      }
      amf_cfg.max_ltr_frames = config::video.amd.amd_ltr_frames;

      // Pre-Analysis sub-system defaults: enable PAQ + TAQ for better quality at same bitrate
      if (amf_cfg.preanalysis && *amf_cfg.preanalysis) {
        amf_cfg.pa_paq_mode = 1;    // CAQ (Content Adaptive Quantization)
        amf_cfg.pa_taq_mode = 2;    // TAQ mode 2 (more aggressive temporal AQ)
        amf_cfg.pa_caq_strength = 1;  // Medium strength
        amf_cfg.pa_activity_type = 1; // YUV activity (better than Y-only)
        amf_cfg.pa_high_motion_quality_boost = 1;  // Auto
      }

      // High motion quality boost: opt-in only. Default nullopt = do not call
      // SetProperty, let the AMD driver pick its default (FFmpeg-aligned).
      // Forcing this on unconditionally was found to expose driver bugs on
      // RDNA4 + Adrenalin 26.5.x (AlkaidLab/foundation-sunshine#666).
      amf_cfg.high_motion_quality_boost_enable = config::video.amd.amd_high_motion_qb;

      // Low latency mode / input queue size / AV1 encoding latency mode:
      // also opt-in to match FFmpeg amfenc behavior. Default nullopt =
      // do not SetProperty, driver picks the default code path.
      amf_cfg.lowlatency_mode = config::video.amd.amd_lowlatency_mode;
      amf_cfg.input_queue_size = config::video.amd.amd_input_queue_size;
      amf_cfg.multi_hw_instance_encode = config::video.amd.amd_multi_hw_instance;
      amf_cfg.av1_encoding_latency_mode = config::video.amd.amd_av1_latency_mode;

      // Apply server-side slices per frame override if configured
      auto effective_config = client_config;
      if (config::video.amd.amd_slices_per_frame > 0) {
        effective_config.slicesPerFrame = std::max(effective_config.slicesPerFrame, config::video.amd.amd_slices_per_frame);
      }

      if (!amf_d3d->create_encoder(amf_cfg, effective_config, colorspace, buffer_format)) return false;

      base.apply_colorspace(colorspace);
      hdr_luminance_analysis_available = false;
      base.set_client_sdr_white(client_config.hdr_capabilities.sdr_white_nits);
      if (base.init_output(static_cast<ID3D11Texture2D *>(amf_d3d->get_input_texture()), client_config.width, client_config.height, colorspace, client_config.videoFormat, is_probe) != 0) {
        return false;
      }

      hdr_luminance_analysis_available = base.hdr_luminance_analysis_available();
      return true;
    }

    int
    convert(platf::img_t &img_base) override {
      int result = base.convert(img_base);
      hdr_luminance_stats = base.hdr_luminance_stats_out;
      return result;
    }

    void
    set_client_sdr_white_nits(float nits) override {
      base.set_client_sdr_white(nits);
    }

  private:
    d3d_base_encode_device base;
    std::unique_ptr<::amf::amf_d3d11> amf_d3d;
    platf::pix_fmt_e buffer_format = platf::pix_fmt_e::unknown;
  };

  capture_e
  display_ddup_vram_t::snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) {
    HRESULT status;
    DXGI_OUTDUPL_FRAME_INFO frame_info;

    const bool use_local_cursor = sync_local_cursor_mode(dup);

    resource_t::pointer res_p {};
    auto capture_status = dup.next_frame(frame_info, timeout, &res_p);
    resource_t res { res_p };

    if (capture_status != capture_e::ok) {
      return capture_status;
    }

    const bool mouse_update_flag = frame_info.LastMouseUpdateTime.QuadPart != 0 || frame_info.PointerShapeBufferSize > 0;
    const bool frame_update_flag = frame_info.LastPresentTime.QuadPart != 0;
    const bool update_flag = mouse_update_flag || frame_update_flag;

    if (!update_flag) {
      return capture_e::timeout;
    }

    std::optional<std::chrono::steady_clock::time_point> frame_timestamp;
    if (auto qpc_displayed = std::max(frame_info.LastPresentTime.QuadPart, frame_info.LastMouseUpdateTime.QuadPart)) {
      // Translate QueryPerformanceCounter() value to steady_clock time point
      frame_timestamp = std::chrono::steady_clock::now() - qpc_time_difference(qpc_counter(), qpc_displayed);
    }

    bool shape_updated;
    if (dup.update_cursor(frame_info, shape_updated) != capture_e::ok) {
      return capture_e::error;
    }
    auto &cursor = dup.cursor;
    if (use_local_cursor) {
      publish_local_cursor(cursor, shape_updated);
    }

    if (shape_updated) {
      normalized_cursor_shape_t normalized;
      if (!normalize_cursor_shape(
            cursor.img_data,
            cursor.shape_info,
            true,
            normalized
          )) {
        return capture_e::error;
      }

      if (!set_cursor_texture(device.get(), cursor_alpha, std::move(normalized.alpha), normalized.info) ||
          !set_cursor_texture(device.get(), cursor_xor, std::move(normalized.xor_mask), normalized.info)) {
        return capture_e::error;
      }
    }

    if (frame_info.LastMouseUpdateTime.QuadPart) {
      cursor_alpha.set_pos(cursor.x, cursor.y,
        width, height, display_rotation, cursor.visible);

      cursor_xor.set_pos(cursor.x, cursor.y,
        width, height, display_rotation, cursor.visible);
    }

    const bool blend_mouse_cursor_flag =
      !use_local_cursor &&
      (cursor_alpha.visible || cursor_xor.visible) &&
      cursor_visible;

    texture2d_t src {};
    if (frame_update_flag) {
      // Get the texture object from this frame
      status = res->QueryInterface(IID_ID3D11Texture2D, (void **) &src);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't query interface [0x"sv << util::hex(status).to_string_view() << ']';
        return capture_e::error;
      }

      D3D11_TEXTURE2D_DESC desc;
      src->GetDesc(&desc);

      // It's possible for our display enumeration to race with mode changes and result in
      // mismatched image pool and desktop texture sizes. If this happens, just reinit again.
      if (desc.Width != width_before_rotation || desc.Height != height_before_rotation) {
        BOOST_LOG(info) << "Capture size changed ["sv << width << 'x' << height << " -> "sv << desc.Width << 'x' << desc.Height << ']';
        return capture_e::reinit;
      }

      // If we don't know the capture format yet, grab it from this texture
      if (capture_format == DXGI_FORMAT_UNKNOWN) {
        capture_format = desc.Format;
        BOOST_LOG(info) << "Capture format ["sv << dxgi_format_to_string(capture_format) << ']';
      }

      // It's also possible for the capture format to change on the fly. If that happens,
      // reinitialize capture to try format detection again and create new images.
      if (capture_format != desc.Format) {
        BOOST_LOG(info) << "Capture format changed ["sv << dxgi_format_to_string(capture_format) << " -> "sv << dxgi_format_to_string(desc.Format) << ']';
        return capture_e::reinit;
      }
    }

    enum class lfa {
      nothing,
      replace_surface_with_img,
      replace_img_with_surface,
      copy_src_to_img,
      copy_src_to_surface,
    };

    enum class ofa {
      forward_last_img,
      copy_last_surface_and_blend_cursor,
      dummy_fallback,
    };

    auto last_frame_action = lfa::nothing;
    auto out_frame_action = ofa::dummy_fallback;

    if (capture_format == DXGI_FORMAT_UNKNOWN) {
      // We don't know the final capture format yet, so we will encode a black dummy image
      last_frame_action = lfa::nothing;
      out_frame_action = ofa::dummy_fallback;
    }
    else {
      if (src) {
        // We got a new frame from DesktopDuplication...
        if (blend_mouse_cursor_flag) {
          // ...and we need to blend the mouse cursor onto it.
          // Copy the frame to intermediate surface so we can blend this and future mouse cursor updates
          // without new frames from DesktopDuplication. We use direct3d surface directly here and not
          // an image from pull_free_image_cb mainly because it's lighter (surface sharing between
          // direct3d devices produce significant memory overhead).
          //
          // The intermediate surface must hold a *cursor-free* copy of the desktop frame: every output
          // image is built as "clean frame copy + this frame's cursor". Blending directly into the image
          // saved in last_frame_variant would bake the cursor into it and leave a trail behind the cursor
          // on subsequent cursor-only updates.
          last_frame_action = lfa::copy_src_to_surface;
          // Copy the intermediate surface to a new image from pull_free_image_cb and blend the mouse cursor onto it.
          out_frame_action = ofa::copy_last_surface_and_blend_cursor;
        }
        else {
          // ...and we don't need to blend the mouse cursor.
          // Copy the frame to a new image from pull_free_image_cb and save the shared pointer to the image
          // in case the mouse cursor appears without a new frame from DesktopDuplication.
          last_frame_action = lfa::copy_src_to_img;
          // Use saved last image shared pointer as output image evading copy.
          out_frame_action = ofa::forward_last_img;
        }
      }
      else if (!std::holds_alternative<std::monostate>(last_frame_variant)) {
        // We didn't get a new frame from DesktopDuplication...
        if (blend_mouse_cursor_flag) {
          // ...but we need to blend the mouse cursor.
          if (std::holds_alternative<std::shared_ptr<platf::img_t>>(last_frame_variant)) {
            // We have the shared pointer of the last image, replace it with intermediate surface
            // while copying contents so we can blend this and future mouse cursor updates.
            last_frame_action = lfa::replace_img_with_surface;
          }
          // Copy the intermediate surface which contains last DesktopDuplication frame
          // to a new image from pull_free_image_cb and blend the mouse cursor onto it.
          out_frame_action = ofa::copy_last_surface_and_blend_cursor;
        }
        else {
          // ...and we don't need to blend the mouse cursor.
          // This happens when the mouse cursor disappears from screen,
          // or there's mouse cursor on screen, but its drawing is disabled in sunshine.
          if (std::holds_alternative<texture2d_t>(last_frame_variant)) {
            // We have the intermediate surface that was used as the mouse cursor blending base.
            // Replace it with an image from pull_free_image_cb copying contents and freeing up the surface memory.
            // Save the shared pointer to the image in case the mouse cursor reappears.
            last_frame_action = lfa::replace_surface_with_img;
          }
          // Use saved last image shared pointer as output image evading copy.
          out_frame_action = ofa::forward_last_img;
        }
      }
    }

    auto create_surface = [&](texture2d_t &surface) -> bool {
      // Try to reuse the old surface if it hasn't been destroyed yet.
      if (old_surface_delayed_destruction) {
        surface.reset(old_surface_delayed_destruction.release());
        return true;
      }

      // Otherwise create a new surface.
      D3D11_TEXTURE2D_DESC t {};
      t.Width = width_before_rotation;
      t.Height = height_before_rotation;
      t.MipLevels = 1;
      t.ArraySize = 1;
      t.SampleDesc.Count = 1;
      t.Usage = D3D11_USAGE_DEFAULT;
      t.Format = capture_format;
      t.BindFlags = 0;
      status = device->CreateTexture2D(&t, nullptr, &surface);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to create frame copy texture [0x"sv << util::hex(status).to_string_view() << ']';
        return false;
      }

      return true;
    };

    auto get_locked_d3d_img = [&](std::shared_ptr<platf::img_t> &img, bool dummy = false) -> std::tuple<std::shared_ptr<img_d3d_t>, texture_lock_helper> {
      auto d3d_img = std::static_pointer_cast<img_d3d_t>(img);

      // Finish creating the image (if it hasn't happened already),
      // also creates synchronization primitives for shared access from multiple direct3d devices.
      if (complete_img(d3d_img.get(), dummy)) return { nullptr, nullptr };

      // This image is shared between capture direct3d device and encoders direct3d devices,
      // we must acquire lock before doing anything to it.
      texture_lock_helper lock_helper(d3d_img->capture_mutex.get());
      if (!lock_helper.lock()) {
        BOOST_LOG(error) << "Failed to lock capture texture";
        return { nullptr, nullptr };
      }

      // Clear the blank flag now that we're ready to capture into the image
      d3d_img->blank = false;

      return { std::move(d3d_img), std::move(lock_helper) };
    };

    switch (last_frame_action) {
      case lfa::nothing: {
        break;
      }

      case lfa::replace_surface_with_img: {
        auto p_surface = std::get_if<texture2d_t>(&last_frame_variant);
        if (!p_surface) {
          BOOST_LOG(error) << "Logical error at " << __FILE__ << ":" << __LINE__;
          return capture_e::error;
        }

        std::shared_ptr<platf::img_t> img;
        if (!pull_free_image_cb(img)) return capture_e::interrupted;

        auto [d3d_img, lock] = get_locked_d3d_img(img);
        if (!d3d_img) return capture_e::error;

        device_ctx->CopyResource(d3d_img->capture_texture.get(), p_surface->get());

        // We delay the destruction of intermediate surface in case the mouse cursor reappears shortly.
        old_surface_delayed_destruction.reset(p_surface->release());
        old_surface_timestamp = std::chrono::steady_clock::now();

        last_frame_variant = img;
        break;
      }

      case lfa::replace_img_with_surface: {
        auto p_img = std::get_if<std::shared_ptr<platf::img_t>>(&last_frame_variant);
        if (!p_img) {
          BOOST_LOG(error) << "Logical error at " << __FILE__ << ":" << __LINE__;
          return capture_e::error;
        }
        auto [d3d_img, lock] = get_locked_d3d_img(*p_img);
        if (!d3d_img) return capture_e::error;

        p_img = nullptr;
        last_frame_variant = texture2d_t {};
        auto &surface = std::get<texture2d_t>(last_frame_variant);
        if (!create_surface(surface)) return capture_e::error;

        device_ctx->CopyResource(surface.get(), d3d_img->capture_texture.get());
        break;
      }

      case lfa::copy_src_to_img: {
        last_frame_variant = {};

        std::shared_ptr<platf::img_t> img;
        if (!pull_free_image_cb(img)) return capture_e::interrupted;

        auto [d3d_img, lock] = get_locked_d3d_img(img);
        if (!d3d_img) return capture_e::error;

        device_ctx->CopyResource(d3d_img->capture_texture.get(), src.get());
        last_frame_variant = img;
        break;
      }

      case lfa::copy_src_to_surface: {
        auto p_surface = std::get_if<texture2d_t>(&last_frame_variant);
        if (!p_surface) {
          last_frame_variant = texture2d_t {};
          p_surface = std::get_if<texture2d_t>(&last_frame_variant);
          if (!create_surface(*p_surface)) return capture_e::error;
        }
        device_ctx->CopyResource(p_surface->get(), src.get());
        break;
      }
    }

    switch (out_frame_action) {
      case ofa::forward_last_img: {
        auto p_img = std::get_if<std::shared_ptr<platf::img_t>>(&last_frame_variant);
        if (!p_img) {
          BOOST_LOG(error) << "Logical error at " << __FILE__ << ":" << __LINE__;
          return capture_e::error;
        }
        img_out = *p_img;
        break;
      }

      case ofa::copy_last_surface_and_blend_cursor: {
        auto p_surface = std::get_if<texture2d_t>(&last_frame_variant);
        if (!p_surface) {
          BOOST_LOG(error) << "Logical error at " << __FILE__ << ":" << __LINE__;
          return capture_e::error;
        }
        if (!blend_mouse_cursor_flag) {
          BOOST_LOG(error) << "Logical error at " << __FILE__ << ":" << __LINE__;
          return capture_e::error;
        }

        if (!pull_free_image_cb(img_out)) return capture_e::interrupted;

        auto [d3d_img, lock] = get_locked_d3d_img(img_out);
        if (!d3d_img) return capture_e::error;

        device_ctx->CopyResource(d3d_img->capture_texture.get(), p_surface->get());
        blend_cursor(d3d_img->capture_rt.get());
        break;
      }

      case ofa::dummy_fallback: {
        if (!pull_free_image_cb(img_out)) return capture_e::interrupted;

        // Clear the image if it has been used as a dummy.
        // It can have the mouse cursor blended onto it.
        auto old_d3d_img = (img_d3d_t *) img_out.get();
        bool reclear_dummy = !old_d3d_img->blank && old_d3d_img->capture_texture;

        auto [d3d_img, lock] = get_locked_d3d_img(img_out, true);
        if (!d3d_img) return capture_e::error;

        if (reclear_dummy) {
          const float rgb_black[] = { 0.0f, 0.0f, 0.0f, 0.0f };
          device_ctx->ClearRenderTargetView(d3d_img->capture_rt.get(), rgb_black);
        }

        if (blend_mouse_cursor_flag) {
          blend_cursor(d3d_img->capture_rt.get());
        }

        break;
      }
    }

    // Perform delayed destruction of the unused surface if the time is due.
    if (old_surface_delayed_destruction && old_surface_timestamp + 10s < std::chrono::steady_clock::now()) {
      old_surface_delayed_destruction.reset();
    }

    if (img_out) {
      img_out->frame_timestamp = frame_timestamp;
    }

    return capture_e::ok;
  }

  capture_e
  display_ddup_vram_t::release_snapshot() {
    return dup.release_frame();
  }

  int
  display_vram_t::init_cursor_pipeline(const ::video::config_t &config) {
    cursor_pipeline_ready = false;
    cursor_white_normalization_enabled = false;
    cursor_white_multiplier.reset();
    cursor_white_multiplier_value = 300.0f / 80.0f;

    D3D11_SAMPLER_DESC sampler_desc {};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler_desc.MinLOD = 0;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;

    auto status = device->CreateSamplerState(&sampler_desc, &sampler_linear);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to create linear sampler state [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    status = device->CreateSamplerState(&sampler_desc, &sampler_point);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to create point sampler state [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    status = device->CreateVertexShader(cursor_vs_hlsl->GetBufferPointer(), cursor_vs_hlsl->GetBufferSize(), nullptr, &cursor_vs);
    if (status) {
      BOOST_LOG(error) << "Failed to create scene vertex shader [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    {
      int32_t rotation_modifier = display_rotation == DXGI_MODE_ROTATION_UNSPECIFIED ? 0 : display_rotation - 1;
      int32_t rotation_data[16 / sizeof(int32_t)] { rotation_modifier };  // aligned to 16-byte
      auto rotation = make_buffer(device.get(), rotation_data);
      if (!rotation) {
        BOOST_LOG(error) << "Failed to create display rotation vertex constant buffer";
        return -1;
      }
      device_ctx->VSSetConstantBuffers(2, 1, &rotation);
    }

    if (config.dynamicRange && is_hdr()) {
      // This shader will normalize scRGB white levels to a user-defined white level
      status = device->CreatePixelShader(cursor_ps_normalize_white_hlsl->GetBufferPointer(), cursor_ps_normalize_white_hlsl->GetBufferSize(), nullptr, &cursor_ps);
      if (status) {
        BOOST_LOG(error) << "Failed to create cursor blending (normalized white) pixel shader [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }

      // Keep the established 300-nit fallback for backends without producer
      // white-level metadata. VDD replaces it with the driver's current value.
      float white_multiplier_data[16 / sizeof(float)] { cursor_white_multiplier_value.load(std::memory_order_relaxed) };  // aligned to 16-byte
      cursor_white_multiplier = make_buffer(device.get(), white_multiplier_data);
      if (!cursor_white_multiplier) {
        BOOST_LOG(warning) << "Failed to create cursor blending (normalized white) white multiplier constant buffer";
        return -1;
      }
      cursor_white_normalization_enabled = true;
    }
    else {
      status = device->CreatePixelShader(cursor_ps_hlsl->GetBufferPointer(), cursor_ps_hlsl->GetBufferSize(), nullptr, &cursor_ps);
      if (status) {
        BOOST_LOG(error) << "Failed to create cursor blending pixel shader [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }
    }

    blend_alpha = make_blend(device.get(), true, false);
    blend_invert = make_blend(device.get(), true, true);
    blend_disable = make_blend(device.get(), false, false);

    if (!blend_disable || !blend_alpha || !blend_invert) {
      return -1;
    }

    device_ctx->OMSetBlendState(blend_disable.get(), nullptr, 0xFFFFFFFFu);
    ID3D11SamplerState *samplers[] = { sampler_linear.get(), sampler_point.get() };
    device_ctx->PSSetSamplers(0, 2, samplers);
    device_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    cursor_pipeline_ready = true;
    return 0;
  }

  void
  display_vram_t::set_cursor_sdr_white_level(UINT32 sdr_white_level_x1000) {
    if (!cursor_white_normalization_enabled || sdr_white_level_x1000 == 0) {
      return;
    }

    const float sdr_white_nits = static_cast<float>(sdr_white_level_x1000) / 1000.0f;
    if (!std::isfinite(sdr_white_nits) || sdr_white_nits < 1.0f || sdr_white_nits > 10000.0f) {
      return;
    }

    const float next_multiplier = sdr_white_nits / 80.0f;
    if (std::abs(next_multiplier - cursor_white_multiplier_value.load(std::memory_order_relaxed)) < 0.0001f) {
      return;
    }

    float white_multiplier_data[16 / sizeof(float)] { next_multiplier };  // aligned to 16-byte
    auto next_buffer = make_buffer(device.get(), white_multiplier_data);
    if (!next_buffer) {
      BOOST_LOG(warning) << "Failed to update cursor SDR white-level multiplier; retaining previous value"sv;
      return;
    }

    cursor_white_multiplier = std::move(next_buffer);
    cursor_white_multiplier_value = next_multiplier;
  }

  std::optional<float>
  display_vram_t::composed_sdr_white_nits() const {
    if (!cursor_white_normalization_enabled) {
      return std::nullopt;
    }
    return cursor_white_multiplier_value.load(std::memory_order_relaxed) * 80.0f;
  }

  void
  display_vram_t::blend_cursor(ID3D11RenderTargetView *capture_rt) {
    device_ctx->VSSetShader(cursor_vs.get(), nullptr, 0);
    device_ctx->PSSetShader(cursor_ps.get(), nullptr, 0);
    if (cursor_white_normalization_enabled && cursor_white_multiplier) {
      ID3D11Buffer *white_multiplier = cursor_white_multiplier.get();
      device_ctx->PSSetConstantBuffers(1, 1, &white_multiplier);
    }
    device_ctx->OMSetRenderTargets(1, &capture_rt, nullptr);

    if (cursor_alpha.texture.get()) {
      // Perform an alpha blending operation
      device_ctx->OMSetBlendState(blend_alpha.get(), nullptr, 0xFFFFFFFFu);

      device_ctx->PSSetShaderResources(0, 1, &cursor_alpha.input_res);
      device_ctx->RSSetViewports(1, &cursor_alpha.cursor_view);
      device_ctx->Draw(3, 0);
    }

    if (cursor_xor.texture.get()) {
      // Perform an invert blending without touching alpha values
      device_ctx->OMSetBlendState(blend_invert.get(), nullptr, 0x00FFFFFFu);

      device_ctx->PSSetShaderResources(0, 1, &cursor_xor.input_res);
      device_ctx->RSSetViewports(1, &cursor_xor.cursor_view);
      device_ctx->Draw(3, 0);
    }

    device_ctx->OMSetBlendState(blend_disable.get(), nullptr, 0xFFFFFFFFu);

    ID3D11RenderTargetView *emptyRenderTarget = nullptr;
    device_ctx->OMSetRenderTargets(1, &emptyRenderTarget, nullptr);
    device_ctx->RSSetViewports(0, nullptr);
    ID3D11ShaderResourceView *emptyShaderResourceView = nullptr;
    device_ctx->PSSetShaderResources(0, 1, &emptyShaderResourceView);
  }

  int
  display_ddup_vram_t::init(const ::video::config_t &config, const std::string &display_name) {
    if (display_base_t::init(config, display_name) || dup.init(this, config)) {
      return -1;
    }

    if (init_cursor_pipeline(config) != 0) {
      return -1;
    }

    return 0;
  }

  int
  display_amd_vram_t::init(const ::video::config_t &config, const std::string &display_name) {
    if (display_base_t::init(config, display_name) || dup.init(this, config, output_index)) {
      BOOST_LOG(error) << "AMD VRAM() failed";
      return -1;
    }
    
    auto status = device->CreateVertexShader(simple_cursor_vs_hlsl->GetBufferPointer(), simple_cursor_vs_hlsl->GetBufferSize(), nullptr, &cursor_vs);
    if (status) {
      BOOST_LOG(error) << "Failed to create simple cursor vertex shader [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }
    status = device->CreatePixelShader(simple_cursor_ps_hlsl->GetBufferPointer(), simple_cursor_ps_hlsl->GetBufferSize(), nullptr, &cursor_ps);
    if (status) {
      BOOST_LOG(error) << "Failed to create simple cursor pixel shader [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }
    
    blend_invert = make_blend(device.get(), true, true);
    blend_disable = make_blend(device.get(), false, false);

    if (!blend_disable || !blend_invert) {
      return -1;
    }
    
    D3D11_BUFFER_DESC buffer_desc {
      sizeof(float[16 / sizeof(float)]),
      D3D11_USAGE_DEFAULT,
      D3D11_BIND_CONSTANT_BUFFER,
      0
    };

    buf_t::pointer cursor_info_p;
    status = device->CreateBuffer(&buffer_desc, nullptr, &cursor_info_p);
    if (status) {
      BOOST_LOG(error) << "Failed to create cursor position buffer: [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }
    cursor_info = buf_t { cursor_info_p };

    return 0;
  }

  /**
   * @brief Get the next frame from the Windows.Graphics.Capture API and copy it into a new snapshot texture.
   * @param pull_free_image_cb call this to get a new free image from the video subsystem.
   * @param img_out the captured frame is returned here
   * @param timeout how long to wait for the next frame
   * @param cursor_visible whether to capture the cursor
   */
  capture_e
  display_amd_vram_t::snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) {
    amf::AMFSurfacePtr output;
    D3D11_TEXTURE2D_DESC desc;

    CURSORINFO pt;
    pt.cbSize = sizeof(CURSORINFO);

    // Check for display configuration change
    auto capture_status = dup.next_frame(timeout, (amf::AMFData **) &output);
    if (capture_status != capture_e::ok) {
      return capture_status;
    }
    dup.capturedSurface = output;

    texture2d_t src = (ID3D11Texture2D *) dup.capturedSurface->GetPlaneAt(0)->GetNative();
    src->GetDesc(&desc);

    // It's possible for our display enumeration to race with mode changes and result in
    // mismatched image pool and desktop texture sizes. If this happens, just reinit again.
    if (desc.Width != width_before_rotation || desc.Height != height_before_rotation) {
      BOOST_LOG(info) << "Capture size changed ["sv << width << 'x' << height << " -> "sv << desc.Width << 'x' << desc.Height << ']';
      return capture_e::reinit;
    }

    // If we don't know the capture format yet, grab it from this texture
    if (capture_format == DXGI_FORMAT_UNKNOWN) {
      capture_format = desc.Format;
      BOOST_LOG(info) << "AMD Capture format ["sv << dxgi_format_to_string(capture_format) << ']';
    }

    // It's also possible for the capture format to change on the fly. If that happens,
    // reinitialize capture to try format detection again and create new images.
    if (capture_format != desc.Format) {
      BOOST_LOG(info) << "AMD Capture format changed ["sv << dxgi_format_to_string(capture_format) << " -> "sv << dxgi_format_to_string(desc.Format) << ']';
      return capture_e::reinit;
    }

    std::shared_ptr<platf::img_t> img;
    if (!pull_free_image_cb(img))
      return capture_e::interrupted;
    
    auto blend_cursor = [&](img_d3d_t &d3d_img) {
      float new_cursor_data[16/ sizeof(float)] = { (float)pt.ptScreenPos.x, (float)pt.ptScreenPos.y, (float)width, (float)height };
      device_ctx->UpdateSubresource(cursor_info.get(), 0, nullptr, &new_cursor_data, 0, 0);
      
      device_ctx->VSSetConstantBuffers(0, 1, &cursor_info);
      device_ctx->VSSetShader(cursor_vs.get(), nullptr, 0);
      device_ctx->PSSetShader(cursor_ps.get(), nullptr, 0);
      device_ctx->OMSetRenderTargets(1, &d3d_img.capture_rt, nullptr);
      device_ctx->IASetInputLayout(nullptr);
      device_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      device_ctx->OMSetBlendState(blend_invert.get(), nullptr, 0x00FFFFFFu);

      device_ctx->Draw(3, 0);

      ID3D11RenderTargetView *emptyRenderTarget = nullptr;
      device_ctx->OMSetRenderTargets(1, &emptyRenderTarget, nullptr);
      device_ctx->RSSetViewports(0, nullptr);
      ID3D11ShaderResourceView *emptyShaderResourceView = nullptr;
      device_ctx->PSSetShaderResources(0, 1, &emptyShaderResourceView);
        device_ctx->OMSetBlendState(blend_disable.get(), nullptr, 0x00FFFFFFu);
    };

    auto d3d_img = std::static_pointer_cast<img_d3d_t>(img);
    d3d_img->blank = false;  // image is always ready for capture
    if (complete_img(d3d_img.get(), false) == 0) {
      texture_lock_helper lock_helper(d3d_img->capture_mutex.get());
      if (lock_helper.lock()) {
        device_ctx->CopyResource(d3d_img->capture_texture.get(), src.get());
        if (cursor_visible && config::input.amf_draw_mouse_cursor) {
          GetCursorInfo(&pt);
          if (pt.flags == CURSOR_SHOWING) {
            blend_cursor(*d3d_img);
          }
        }
      
      }
      else {
        return capture_e::error;
      }
    }
    else {
      return capture_e::error;
    }
    
    img_out = img;
    if (img_out) {
      img_out->frame_timestamp = std::chrono::steady_clock::now();
    }

    src.release();
    return capture_e::ok;
  }

  capture_e
  display_amd_vram_t::release_snapshot() {
    dup.release_frame();
    return capture_e::ok;
  }

  /**
   * Get the next frame from the Windows.Graphics.Capture API and copy it into a new snapshot texture.
   * @param pull_free_image_cb call this to get a new free image from the video subsystem.
   * @param img_out the captured frame is returned here
   * @param timeout how long to wait for the next frame
   * @param cursor_visible
   */
  capture_e
  display_wgc_vram_t::snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) {
    // Check if window is still valid (if capturing a window)
    // If window becomes invalid (closed, minimized, hidden), fall back to display capture
    if (!dup.is_window_valid()) {
      BOOST_LOG(warning) << "Captured window is no longer valid (closed, minimized, or hidden), falling back to display capture"sv;
      return capture_e::reinit;
    }
    
    texture2d_t src;
    uint64_t frame_qpc;
    dup.set_cursor_visible(cursor_visible);
    auto capture_status = dup.next_frame(timeout, &src, frame_qpc);
    if (capture_status != capture_e::ok) {
      // If we're capturing a window and getting timeouts/errors, check if window is still valid
      if (dup.captured_window_hwnd != nullptr) {
        // Simplified: Any error or timeout means window might have changed, check validity
        if (!dup.is_window_valid()) {
          BOOST_LOG(warning) << "Captured window is no longer valid, reinitializing capture"sv;
          return capture_e::reinit;
        }
      }
      return capture_status;
    }

    auto frame_timestamp = std::chrono::steady_clock::now() - qpc_time_difference(qpc_counter(), frame_qpc);
    D3D11_TEXTURE2D_DESC desc;
    src->GetDesc(&desc);

    // Get the actual captured frame dimensions
    int frame_width = static_cast<int>(desc.Width);
    int frame_height = static_cast<int>(desc.Height);
    
    // For window capture, check if size changed and handle it
    if (dup.captured_window_hwnd != nullptr) {
      int expected_width = dup.window_capture_width > 0 ? dup.window_capture_width : width_before_rotation;
      int expected_height = dup.window_capture_height > 0 ? dup.window_capture_height : height_before_rotation;
      
      if (frame_width != expected_width || frame_height != expected_height) {
        BOOST_LOG(info) << "Window capture size changed ["sv << expected_width << 'x' << expected_height 
                         << " -> "sv << frame_width << 'x' << frame_height << ']';
        // Update stored dimensions
        dup.window_capture_width = frame_width;
        dup.window_capture_height = frame_height;
        // Trigger reinit to recreate all resources (images, textures, etc.) with new size
        return capture_e::reinit;
      }
    }
    else {
      // For display capture with WGC, the frame dimensions are in "display orientation"
      // (i.e., after rotation). Our `width`/`height` are derived from DesktopCoordinates
      // and match that orientation. Using width_before_rotation/height_before_rotation
      // here can cause an infinite reinit loop on rotation.
      if (frame_width != width || frame_height != height) {
        BOOST_LOG(info) << "Capture size changed ["sv << width << 'x' << height << " -> "sv << frame_width << 'x' << frame_height << ']';
        return capture_e::reinit;
      }
    }

    // It's also possible for the capture format to change on the fly. If that happens,
    // reinitialize capture to try format detection again and create new images.
    if (capture_format != desc.Format) {
      BOOST_LOG(info) << "Capture format changed ["sv << dxgi_format_to_string(capture_format) << " -> "sv << dxgi_format_to_string(desc.Format) << ']';
      return capture_e::reinit;
    }

    std::shared_ptr<platf::img_t> img;
    if (!pull_free_image_cb(img))
      return capture_e::interrupted;

    auto d3d_img = std::static_pointer_cast<img_d3d_t>(img);
    d3d_img->blank = false;  // image is always ready for capture
    if (complete_img(d3d_img.get(), false) == 0) {
      texture_lock_helper lock_helper(d3d_img->capture_mutex.get());
      if (lock_helper.lock()) {
        device_ctx->CopyResource(d3d_img->capture_texture.get(), src.get());
      }
      else {
        BOOST_LOG(error) << "Failed to lock capture texture";
        return capture_e::error;
      }
    }
    else {
      return capture_e::error;
    }
    img_out = img;
    if (img_out) {
      img_out->frame_timestamp = frame_timestamp;
    }

    return capture_e::ok;
  }

  capture_e
  display_wgc_vram_t::release_snapshot() {
    return dup.release_frame();
  }

  std::shared_ptr<platf::img_t>
  display_wgc_vram_t::alloc_img() {
    auto img = std::make_shared<img_d3d_t>();
    
    // For window capture, use window capture dimensions; for display capture, use display dimensions
    int img_width = dup.window_capture_width > 0 ? dup.window_capture_width : width;
    int img_height = dup.window_capture_height > 0 ? dup.window_capture_height : height;
    
    img->width = img_width;
    img->height = img_height;
    img->id = next_image_id++;
    img->blank = true;

    return img;
  }

  int
  display_wgc_vram_t::init(const ::video::config_t &config, const std::string &display_name) {
    if (display_base_t::init(config, display_name) || dup.init(this, config))
      return -1;

    // WGC frames are typically delivered in the current display orientation.
    // The DXGI rotation flag comes from the output descriptor and is needed for DDX,
    // but for WGC it can lead to applying rotation twice (client sees flipped/stretched).
    if (display_rotation != DXGI_MODE_ROTATION_UNSPECIFIED &&
        display_rotation != DXGI_MODE_ROTATION_IDENTITY) {
      BOOST_LOG(info) << "WGC: disabling DXGI rotation handling for oriented frames";
      display_rotation = DXGI_MODE_ROTATION_UNSPECIFIED;
      width_before_rotation = width;
      height_before_rotation = height;
    }

    return 0;
  }

  std::shared_ptr<platf::img_t>
  display_vram_t::alloc_img() {
    auto img = std::make_shared<img_d3d_t>();

    // Initialize format-independent fields
    img->width = width_before_rotation;
    img->height = height_before_rotation;
    img->id = next_image_id++;
    img->blank = true;

    return img;
  }

  // This cannot use ID3D11DeviceContext because it can be called concurrently by the encoding thread
  int
  display_vram_t::complete_img(platf::img_t *img_base, bool dummy) {
    auto img = (img_d3d_t *) img_base;

    // If this already has a capture texture and it's not switching dummy state, nothing to do
    if (!img->borrowed_vdd_texture && !img->borrowed_vdd_frame &&
        img->capture_texture && img->capture_rt && img->capture_mutex &&
        img->encoder_texture_handle && img->dummy == dummy) {
      return 0;
    }

    // If this is not a dummy image, we must know the format by now
    if (!dummy && capture_format == DXGI_FORMAT_UNKNOWN) {
      BOOST_LOG(error) << "display_vram_t::complete_img() called with unknown capture format!";
      return -1;
    }

    // Reset the image (in case this was previously a dummy or borrowed VDD slot)
    if (!img->abandon_borrowed_vdd_frame()) {
      return -1;
    }
    img->capture_texture.reset();
    img->capture_rt.reset();
    img->capture_mutex.reset();
    img->data = nullptr;
    if (img->encoder_texture_handle) {
      CloseHandle(img->encoder_texture_handle);
      img->encoder_texture_handle = NULL;
    }

    // Initialize format-dependent fields
    img->pixel_pitch = get_pixel_pitch();
    img->row_pitch = img->pixel_pitch * img->width;
    img->dummy = dummy;
    img->format = (capture_format == DXGI_FORMAT_UNKNOWN) ? DXGI_FORMAT_B8G8R8A8_UNORM : capture_format;
    img->linear_gamma = capture_linear_gamma;
    img->borrowed_vdd_texture = false;

    D3D11_TEXTURE2D_DESC t {};
    t.Width = img->width;
    t.Height = img->height;
    t.MipLevels = 1;
    t.ArraySize = 1;
    t.SampleDesc.Count = 1;
    t.Usage = D3D11_USAGE_DEFAULT;
    t.Format = img->format;
    t.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    t.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    auto status = device->CreateTexture2D(&t, nullptr, &img->capture_texture);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to create img buf texture [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    status = device->CreateRenderTargetView(img->capture_texture.get(), nullptr, &img->capture_rt);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to create render target view [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    // Get the keyed mutex to synchronize with the encoding code
    status = img->capture_texture->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **) &img->capture_mutex);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to query IDXGIKeyedMutex [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    resource1_t resource;
    status = img->capture_texture->QueryInterface(__uuidof(IDXGIResource1), (void **) &resource);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to query IDXGIResource1 [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    // Create a handle for the encoder device to use to open this texture
    status = resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, nullptr, &img->encoder_texture_handle);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to create shared texture handle [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    img->data = (std::uint8_t *) img->capture_texture.get();

    return 0;
  }

  // This cannot use ID3D11DeviceContext because it can be called concurrently by the encoding thread
  /**
   * @memberof platf::dxgi::display_vram_t
   */
  int
  display_vram_t::dummy_img(platf::img_t *img_base) {
    return complete_img(img_base, true);
  }

  std::vector<DXGI_FORMAT>
  display_vram_t::get_supported_capture_formats() {
    return {
      // scRGB FP16 is the ideal format for Wide Color Gamut and Advanced Color
      // displays (both SDR and HDR). This format uses linear gamma, so we will
      // use a linear->PQ shader for HDR and a linear->sRGB shader for SDR.
      DXGI_FORMAT_R16G16B16A16_FLOAT,

      // DXGI_FORMAT_R10G10B10A2_UNORM seems like it might give us frames already
      // converted to SMPTE 2084 PQ, however it seems to actually just clamp the
      // scRGB FP16 values that DWM is using when the desktop format is scRGB FP16.
      //
      // If there is a case where the desktop format is really SMPTE 2084 PQ, it
      // might make sense to support capturing it without conversion to scRGB,
      // but we avoid it for now.

      // We include the 8-bit modes too for when the display is in SDR mode,
      // while the client stream is HDR-capable. These UNORM formats can
      // use our normal pixel shaders that expect sRGB input.
      DXGI_FORMAT_B8G8R8A8_UNORM,
      DXGI_FORMAT_B8G8R8X8_UNORM,
      DXGI_FORMAT_R8G8B8A8_UNORM,
    };
  }

  /**
   * @brief Check that a given codec is supported by the display device.
   * @param name The FFmpeg codec name (or similar for non-FFmpeg codecs).
   * @param config The codec configuration.
   * @return `true` if supported, `false` otherwise.
   */
  bool
  display_vram_t::is_codec_supported(std::string_view name, const ::video::config_t &config) {
    DXGI_ADAPTER_DESC adapter_desc;
    adapter->GetDesc(&adapter_desc);

    if (adapter_desc.VendorId == 0x1002) {  // AMD
      // If it's not an AMF encoder, it's not compatible with an AMD GPU
      if (!boost::algorithm::ends_with(name, "_amf")) {
        return false;
      }

      // Perform AMF version checks if we're using an AMD GPU. This check is placed in display_vram_t
      // to avoid hitting the display_ram_t path which uses software encoding and doesn't touch AMF.
      HMODULE amfrt = LoadLibraryW(AMF_DLL_NAME);
      if (amfrt) {
        auto unload_amfrt = util::fail_guard([amfrt]() {
          FreeLibrary(amfrt);
        });

        auto fnAMFQueryVersion = (AMFQueryVersion_Fn) GetProcAddress(amfrt, AMF_QUERY_VERSION_FUNCTION_NAME);
        if (fnAMFQueryVersion) {
          amf_uint64 version;
          auto result = fnAMFQueryVersion(&version);
          if (result == AMF_OK) {
            if (config.videoFormat == 2 && version < AMF_MAKE_FULL_VERSION(1, 4, 30, 0)) {
              // AMF 1.4.30 adds ultra low latency mode for AV1. Don't use AV1 on earlier versions.
              // This corresponds to driver version 23.5.2 (23.10.01.45) or newer.
              BOOST_LOG(warning) << "AV1 encoding is disabled on AMF version "sv
                                 << AMF_GET_MAJOR_VERSION(version) << '.'
                                 << AMF_GET_MINOR_VERSION(version) << '.'
                                 << AMF_GET_SUBMINOR_VERSION(version) << '.'
                                 << AMF_GET_BUILD_VERSION(version);
              BOOST_LOG(warning) << "If your AMD GPU supports AV1 encoding, update your graphics drivers!"sv;
              return false;
            }
            else if (config.dynamicRange && version < AMF_MAKE_FULL_VERSION(1, 4, 23, 0)) {
              // Older versions of the AMD AMF runtime can crash when fed P010 surfaces.
              // Fail if AMF version is below 1.4.23 where HEVC Main10 encoding was introduced.
              // AMF 1.4.23 corresponds to driver version 21.12.1 (21.40.11.03) or newer.
              BOOST_LOG(warning) << "HDR encoding is disabled on AMF version "sv
                                 << AMF_GET_MAJOR_VERSION(version) << '.'
                                 << AMF_GET_MINOR_VERSION(version) << '.'
                                 << AMF_GET_SUBMINOR_VERSION(version) << '.'
                                 << AMF_GET_BUILD_VERSION(version);
              BOOST_LOG(warning) << "If your AMD GPU supports HEVC Main10 encoding, update your graphics drivers!"sv;
              return false;
            }
          }
          else {
            BOOST_LOG(warning) << "AMFQueryVersion() failed: "sv << result;
          }
        }
        else {
          BOOST_LOG(warning) << "AMF DLL missing export: "sv << AMF_QUERY_VERSION_FUNCTION_NAME;
        }
      }
      else {
        BOOST_LOG(warning) << "Detected AMD GPU but AMF failed to load"sv;
      }
    }
    else if (adapter_desc.VendorId == 0x8086) {  // Intel
      // If it's not a QSV encoder, it's not compatible with an Intel GPU
      if (!boost::algorithm::ends_with(name, "_qsv")) {
        return false;
      }
      if (config.chromaSamplingType == 1) {
        if (config.videoFormat == 0 || config.videoFormat == 2) {
          // QSV doesn't support 4:4:4 in H.264 or AV1
          return false;
        }
        // TODO: Blacklist HEVC 4:4:4 based on adapter model
      }
    }
    else if (adapter_desc.VendorId == 0x10de) {  // Nvidia
      // If it's not an NVENC encoder, it's not compatible with an Nvidia GPU
      if (!boost::algorithm::ends_with(name, "_nvenc")) {
        return false;
      }
    }
    else {
      BOOST_LOG(warning) << "Unknown GPU vendor ID: " << util::hex(adapter_desc.VendorId).to_string_view();
    }

    return true;
  }

  std::unique_ptr<avcodec_encode_device_t>
  display_vram_t::make_avcodec_encode_device(pix_fmt_e pix_fmt) {
    auto device = std::make_unique<d3d_avcodec_encode_device_t>();
    if (device->init(shared_from_this(), adapter.get(), pix_fmt) != 0) {
      return nullptr;
    }
    return device;
  }

  std::unique_ptr<nvenc_encode_device_t>
  display_vram_t::make_nvenc_encode_device(pix_fmt_e pix_fmt) {
    // For hybrid graphics laptops, NVENC encoder requires NVIDIA GPU,
    // but display capture may use integrated graphics (built-in screen).
    // We need to find the NVIDIA adapter for encoding, not the capture adapter.
    adapter_t::pointer nvenc_adapter_p = nullptr;
    adapter_t nvenc_adapter;  // Smart pointer to manage adapter lifetime if we find a different one
    
    // Check if current adapter is NVIDIA
    DXGI_ADAPTER_DESC adapter_desc;
    adapter->GetDesc(&adapter_desc);
    
    if (adapter_desc.VendorId == 0x10de) {  // NVIDIA
      // Current adapter is already NVIDIA, use it
      nvenc_adapter_p = adapter.get();
    }
    else {
      // Current adapter is not NVIDIA (likely integrated graphics),
      // find the NVIDIA adapter for encoding
      factory1_t factory;
      HRESULT status = CreateDXGIFactory1(IID_IDXGIFactory1, (void **) &factory);
      if (SUCCEEDED(status)) {
        adapter_t::pointer adapter_p;
        for (int x = 0; factory->EnumAdapters1(x, &adapter_p) != DXGI_ERROR_NOT_FOUND; ++x) {
          dxgi::adapter_t adapter_tmp { adapter_p };
          DXGI_ADAPTER_DESC1 adapter_desc1;
          adapter_tmp->GetDesc1(&adapter_desc1);
          
          if (adapter_desc1.VendorId == 0x10de) {  // NVIDIA
            // Found NVIDIA adapter, use it
            nvenc_adapter = std::move(adapter_tmp);
            nvenc_adapter_p = nvenc_adapter.get();
            BOOST_LOG(info) << "Found NVIDIA GPU for NVENC encoding: " << platf::to_utf8(adapter_desc1.Description)
                            << " (display capture uses: " << platf::to_utf8(adapter_desc.Description) << ")";
            break;
          }
        }
      }
      
      if (!nvenc_adapter_p) {
        BOOST_LOG(error) << "Failed to find NVIDIA GPU adapter for NVENC encoding. "
                         << "Current adapter (VendorId: 0x" << util::hex(adapter_desc.VendorId).to_string_view()
                         << ") does not support NVENC.";
        return nullptr;
      }
    }
    
    auto device = std::make_unique<d3d_nvenc_encode_device_t>();
    if (!device->init_device(shared_from_this(), nvenc_adapter_p, pix_fmt)) {
      return nullptr;
    }
    
    return device;
  }

  std::unique_ptr<amf_encode_device_t>
  display_vram_t::make_amf_encode_device(pix_fmt_e pix_fmt) {
    // Find AMD adapter for AMF encoding
    adapter_t::pointer amf_adapter_p = nullptr;
    adapter_t amf_adapter;

    DXGI_ADAPTER_DESC adapter_desc;
    adapter->GetDesc(&adapter_desc);

    if (adapter_desc.VendorId == 0x1002) {  // AMD
      amf_adapter_p = adapter.get();
    }
    else {
      factory1_t factory;
      HRESULT status = CreateDXGIFactory1(IID_IDXGIFactory1, (void **) &factory);
      if (SUCCEEDED(status)) {
        adapter_t::pointer adapter_p;
        for (int x = 0; factory->EnumAdapters1(x, &adapter_p) != DXGI_ERROR_NOT_FOUND; ++x) {
          dxgi::adapter_t adapter_tmp { adapter_p };
          DXGI_ADAPTER_DESC1 adapter_desc1;
          adapter_tmp->GetDesc1(&adapter_desc1);

          if (adapter_desc1.VendorId == 0x1002) {  // AMD
            amf_adapter = std::move(adapter_tmp);
            amf_adapter_p = amf_adapter.get();
            BOOST_LOG(info) << "Found AMD GPU for AMF encoding: " << platf::to_utf8(adapter_desc1.Description);
            break;
          }
        }
      }

      if (!amf_adapter_p) {
        BOOST_LOG(error) << "Failed to find AMD GPU adapter for AMF encoding.";
        return nullptr;
      }
    }

    auto device = std::make_unique<d3d_amf_encode_device_t>();
    if (!device->init_device(shared_from_this(), amf_adapter_p, pix_fmt)) {
      return nullptr;
    }

    return device;
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
