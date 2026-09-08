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
#include "d3d12/d3d12_hdr_analysis.h"
#include "display_cursor.h"
#include "display_vram_internal.h"
#include "display_vram_shaders.h"
#include "hdr_analysis_result.h"
#include "misc.h"
#include "video_pipeline_telemetry.h"
#include "pre_encode_filter.h"
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
    // Coprime with the four-frame analysis cadence, so sampling cannot miss
    // every analysis frame because the two counters start at different phases.
    constexpr uint64_t vram_gpu_timing_sample_interval = 31;
    uint64_t
    timing_sample_interval() {
      const char *raw = std::getenv("SUNSHINE_VRAM_TIMING_SAMPLE_INTERVAL");
      if (!raw) return vram_gpu_timing_sample_interval;
      char *end = nullptr;
      const auto value = std::strtoul(raw, &end, 10);
      return end != raw && *end == '\0' && value >= 1 && value <= 1000 ? value : vram_gpu_timing_sample_interval;
    }
    constexpr size_t vram_gpu_timing_max_pending = 8;

    using timing_bucket_t = telemetry::sample_window_t;

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

  class d3d_base_encode_device final {
    // GPU contract shared by the PQ/HLG converters and HDR analysis shaders.
    struct alignas(16) HdrPreEncodeParams {
      float nominalPeakNits;
      float hlgSystemGamma;
      // SDR band re-anchoring: gain applied to scRGB levels at or below
      // sdrBandTopScrgb (Windows SDR white / 80), then monotonically fading
      // back to 1.0 in log-luminance space.
      // gain 1.0 / top 0 leaves the signal untouched.
      float sdrBandGain;
      float sdrBandTopScrgb;
    };
    static_assert(sizeof(HdrPreEncodeParams) == 16);

    struct HdrPreEncodeState {
      buf_t constantBuffer;
      HdrPreEncodeParams params { 0.0f, 1.0f, 1.0f, 0.0f };
      // Client-reported SDR reference white; 0 = no transform requested.
      float clientSdrWhiteNits = 0.0f;

      explicit operator bool() const {
        return bool(constantBuffer);
      }
    };

    // Hides whether pass 1 reads a full scRGB frame or converter-produced cell
    // statistics. Consumers only see the common analysis contract.
    struct HdrAnalysisSource {
      ID3D11ShaderResourceView *statistics = nullptr;
      ID3D11ShaderResourceView *pqAverage = nullptr;
      ID3D11Buffer *parameters = nullptr;

      explicit operator bool() const {
        return statistics && parameters;
      }
    };

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
      telemetry::d3d11_stage_sample_t<query_t> m0;
      query_t end;
      uint64_t source_frame = 0;
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
      telemetry::m0_pipeline_metrics_t m0;
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
        m0.reset();
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

    bool
    video_backend_available() const {
      const auto vram = std::dynamic_pointer_cast<display_vram_t>(display);
      return !vram || !vram->video_backend_selection ||
             vram->video_backend_selection->pipeline_available();
    }

    int
    convert(platf::img_t &img_base) {
      if (!video_backend_available()) return -1;
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
        const auto video_frame_index = video_frame_counter++;
        auto &img_ctx = img_ctx_map[img.id];

        // Open the shared capture texture with our ID3D11Device
        if (initialize_image_context(img, img_ctx)) {
          return -1;
        }

        // Poll the previous analysis result before taking the capture mutex.
        // Both readbacks are drained unconditionally: enabling the D3D12 path
        // does not retire the D3D11 one, which still serves frames the compute
        // converter could not snapshot, and an unread D3D11 staging buffer
        // would otherwise stay pending forever.
        if (d3d12_hdr_analysis && d3d12_hdr_analysis->available()) {
          read_d3d12_hdr_analysis_results(video_frame_index);
        }
        if (!video_backend_available()) return -1;
        if (hdr_analysis_pending) {
          read_hdr_analysis_results(video_frame_index);
        }
        if (hdr_luminance_stats_out.valid && !runtime_status.scene_metadata_active) {
          runtime_status.scene_metadata_active = true;
          ::video::update_hdr_pipeline_status(runtime_status_id, runtime_status);
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
          gpu_timing->source_frame = video_frame_index;
          gpu_timing->borrowed_vdd = img.borrowed_vdd_texture;
        }

        bool capture_mutex_released = false;
        auto release_capture_mutex = [&]() -> bool {
          if (capture_mutex_released) {
            return true;
          }
          bool released = false;
          if (borrowed_vdd_frame) {
            released = img.release_borrowed_vdd_after_convert(img_ctx.encoder_mutex.get());
          }
          else {
            released = SUCCEEDED(img_ctx.encoder_mutex->ReleaseSync(encoder_release_key));
          }
          capture_mutex_released = released;
          return released;
        };

        ID3D11Texture2D *conversion_input_texture = img_ctx.encoder_texture.get();
        ID3D11ShaderResourceView *conversion_input_srv = img_ctx.encoder_input_res.get();
        DXGI_FORMAT conversion_input_format = img.format;
        auto conversion_input_semantic = img.frame_desc;

        if (pre_encode_filter) {
          auto source_contract = display->capture_contract;
          source_contract.require_private_handoff = false;
          if (!frame_satisfies_capture_contract(source_contract, img.frame_desc)) {
            release_capture_mutex();
            BOOST_LOG(error) << "Pre-encode filter rejected captured frame contract"sv;
            update_synthetic_hdr_runtime_status(false, "capture_contract_mismatch");
            return -1;
          }
          if (!prepare_filter_handoff(img_ctx.encoder_texture.get(), img.frame_desc)) {
            release_capture_mutex();
            update_synthetic_hdr_runtime_status(false, "filter_handoff_failed");
            return -1;
          }
          device_ctx->CopyResource(filter_handoff_texture.get(), img_ctx.encoder_texture.get());
          if (!release_capture_mutex()) {
            return -1;
          }

          auto handoff_semantic = img.frame_desc;
          handoff_semantic.borrowed = false;
          const auto filter_result = pre_encode_filter->process({
            .texture = filter_handoff_texture.get(),
            .srv = filter_handoff_srv.get(),
            .format = img.format,
            .semantic = handoff_semantic,
            .width = static_cast<std::uint32_t>(img.width),
            .height = static_cast<std::uint32_t>(img.height),
          });
          if (filter_result.status != filter_status_e::ready ||
              !filter_result.frame.texture || !filter_result.frame.srv) {
            BOOST_LOG(error) << "Pre-encode filter failed: "sv << filter_result.reason;
            update_synthetic_hdr_runtime_status(false, filter_result.reason);
            return -1;
          }
          update_synthetic_hdr_runtime_status(true);
          conversion_input_texture = filter_result.frame.texture;
          conversion_input_srv = filter_result.frame.srv;
          conversion_input_format = filter_result.frame.format;
          conversion_input_semantic = filter_result.frame.semantic;
        }

        const bool input_is_linear_fp16 =
          conversion_input_semantic.domain == frame_domain_e::linear_scrgb &&
          conversion_input_semantic.encoding == pixel_encoding_class_e::float16 &&
          conversion_input_format == DXGI_FORMAT_R16G16B16A16_FLOAT;
        const bool can_analyze_hdr_frame = hdr_analysis_enabled && input_is_linear_fp16;

        auto draw = [&](auto &input, auto &y_or_yuv_viewports, auto &uv_viewport) {
          device_ctx->PSSetShaderResources(0, 1, &input);

          // The SDR white gain buffer may be rebuilt at a frame boundary. Bind
          // the current buffer here so the pixel-shader fallback sees updates
          // just like the compute-shader path.
          if (hdr_pre_encode) {
            ID3D11Buffer *pre_encode_cbuf = hdr_pre_encode.constantBuffer.get();
            device_ctx->PSSetConstantBuffers(3, 1, &pre_encode_cbuf);
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
          const bool use_linear_shader = input_is_linear_fp16;

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
        const bool hdr_analysis_cadence_due =
          can_analyze_hdr_frame && should_dispatch_hdr_analysis();
        const bool use_d3d12_hdr_analysis =
          d3d12_hdr_analysis && d3d12_hdr_analysis->available();
        std::optional<d3d12::writable_snapshot_t> d3d12_snapshot;
        if (hdr_analysis_cadence_due && use_d3d12_hdr_analysis) {
          d3d12_snapshot =
            d3d12_hdr_analysis->try_acquire_snapshot();
          if (!d3d12_hdr_analysis->available()) {
            if (auto vram = std::dynamic_pointer_cast<display_vram_t>(display)) {
              vram->disable_d3d12_analysis(d3d12_hdr_analysis->failure_stage(),
                d3d12_hdr_analysis->failure_hresult());
            }
          }
        }
        const bool hdr_analysis_due =
          hdr_analysis_cadence_due &&
          (use_d3d12_hdr_analysis ?
             d3d12_snapshot.has_value() :
             !hdr_analysis_pending);
        if (gpu_timing && hdr_analysis_due) gpu_timing->m0.mark_analysis_frame();
        if (vram_timing_enabled) {
          gpu_timing_stats.m0.analysis_due += hdr_analysis_cadence_due;
          analysis_skipped_busy +=
            hdr_analysis_cadence_due && !hdr_analysis_due;
        }
        bool cs_used = false;
        bool hdr_analysis_snapshot_written = false;
        update_hdr_pre_encode_transform();
        if (cs_path_active) {
          if (cs_for_p010) {
            // HDR P010: shader expects linear scRGB FP16 input.
            if (input_is_linear_fp16) {
              const bool write_hdr_analysis_snapshot =
                hdr_analysis_due && hdr_analysis_snapshot_enabled;
              cs_t &shader = write_hdr_analysis_snapshot ?
                               (cs_is_scaled ? cs_p010_scaled_hdr_analysis : cs_p010_hdr_analysis) :
                               (cs_is_scaled ? cs_p010_scaled : cs_p010);
              // When a D3D12 slot was acquired, the converter writes the cell
              // statistics straight into the shared snapshot texture. That keeps
              // the hybrid path copy-free. submit() hands the producer batch to
              // D3D12 with a fence signal and asynchronous flush.
              cs_used = try_dispatch_cs_convert(
                conversion_input_srv,
                shader,
                write_hdr_analysis_snapshot,
                gpu_timing,
                d3d12_snapshot ? d3d12_snapshot->uav : nullptr,
                d3d12_snapshot ? d3d12_snapshot->pq_uav : nullptr);
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
                conversion_input_srv, shader, false, gpu_timing);
            }
          }
        }
        if (!cs_used) {
          draw(conversion_input_srv, out_Y_or_YUV_viewports, out_UV_viewport);
          mark_draw_gpu_timing(gpu_timing);
        }
        if (vram_timing_enabled) {
          gpu_timing_stats.m0.record_conversion_path(cs_used, cs_writes_output_directly);
        }
        if (d3d12_snapshot && !hdr_analysis_snapshot_written) {
          // The converter never ran (pixel-shader path, or the CS variant was
          // unavailable), so the shared snapshot holds nothing. Return the slot.
          (void) d3d12_hdr_analysis->cancel_snapshot(
            *d3d12_snapshot);
          d3d12_snapshot.reset();
        }

        ID3D11ShaderResourceView *emptyShaderResourceView = nullptr;
        device_ctx->PSSetShaderResources(0, 1, &emptyShaderResourceView);

        const HdrAnalysisSource hdr_analysis_source = hdr_analysis_due && !d3d12_snapshot && !hdr_analysis_pending
                                                        ? prepare_hdr_analysis_source(
                                                            hdr_analysis_snapshot_written,
                                                            conversion_input_texture,
                                                            gpu_timing)
                                                        : HdrAnalysisSource {};

        // Release encoder mutex to allow capture code to reuse this image.
        if (!release_capture_mutex()) {
          if (d3d12_snapshot) {
            // No compute work has been queued yet, even if D3D11 wrote the slot.
            (void) d3d12_hdr_analysis->cancel_snapshot(*d3d12_snapshot);
          }
          finish_gpu_timing_sample(gpu_timing, std::move(gpu_timing_sample));
          return -1;
        }
        if (d3d12_snapshot || hdr_analysis_source) {
          if (vram_timing_enabled) {
            ++gpu_timing_stats.m0.analysis_dispatched;
          }
          if (d3d12_snapshot) {
            if (!d3d12_hdr_analysis->submit(
                  *d3d12_snapshot,
                  video_frame_index,
                  gpu_timing != nullptr)) {
              if (auto display_vram =
                    std::dynamic_pointer_cast<display_vram_t>(
                      display)) {
                display_vram->disable_d3d12_analysis(
                  d3d12_hdr_analysis->failure_stage(),
                  d3d12_hdr_analysis->failure_hresult());
              }
              d3d12_hdr_analysis->disable();
            }
          }
          else {
            dispatch_hdr_analysis(
              hdr_analysis_source,
              video_frame_index,
              gpu_timing);
          }
        }
        finish_gpu_timing_sample(gpu_timing, std::move(gpu_timing_sample));
        if (vram_timing_enabled) {
          cpu_submit_timing.add(elapsed_ms(std::chrono::steady_clock::now() - submit_start));
          if (can_analyze_hdr_frame && hdr_analysis_last_completed_frame) {
            analysis_result_age_timing.add(static_cast<double>(
              video_frame_index - *hdr_analysis_last_completed_frame));
          }
          log_cpu_timing();
        }
      }

      return video_backend_available() ? 0 : -1;
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
    configure_hdr_pre_encode(bool use_pq_shader, bool use_hlg_shader, bool is_probe) {
      hdr_pre_encode.constantBuffer.reset();
      hdr_pre_encode.params = { 0.0f, 1.0f, 1.0f, 0.0f };
      ID3D11Buffer *null_cbuf = nullptr;
      device_ctx->PSSetConstantBuffers(3, 1, &null_cbuf);

      float analysis_max_nits = 10000.0f;
      if (use_pq_shader || use_hlg_shader) {
        SS_HDR_METADATA metadata {};
        // Use the effective capture-display metadata as the single source of
        // truth. VDD reports the client-mapped capabilities here; a physical
        // output reports the values after Windows applies its HDR color profile.
        const bool has_display_peak =
          display->get_hdr_metadata(metadata) && metadata.maxDisplayLuminance > 0;
        // HLG needs the nominal display peak for its inverse OOTF. PQ does not,
        // but uses the same constant-buffer layout for SDR-band re-anchoring.
        const float peak_nits = use_hlg_shader
                                  ? (has_display_peak
                                       ? static_cast<float>(metadata.maxDisplayLuminance)
                                       : 1000.0f)
                                  : 10000.0f;
        const float system_gamma = use_hlg_shader
                                     ? ::video::hlg_system_gamma(peak_nits)
                                     : 1.0f;
        const HdrPreEncodeParams params {
          peak_nits,
          system_gamma,
          1.0f,
          0.0f,
        };

        auto hdr_params = make_buffer(device.get(), params);
        if (!hdr_params) {
          BOOST_LOG(error) << "Failed to create HDR pre-encode parameter buffer";
          return -1;
        }

        ID3D11Buffer *hdr_params_p = hdr_params.get();
        device_ctx->PSSetConstantBuffers(3, 1, &hdr_params_p);
        hdr_pre_encode.constantBuffer = std::move(hdr_params);
        hdr_pre_encode.params = params;
        // Vivid statistics must describe the encoded HLG range, not scRGB
        // headroom that cannot be represented by the nominal HLG signal.
        if (use_hlg_shader) {
          analysis_max_nits = std::min(peak_nits, 10000.0f);
          BOOST_LOG(is_probe ? debug : info)
            << "HLG conversion: BT.2100 inverse OOTF, nominal display peak "
            << peak_nits << " nits, system gamma " << system_gamma
            << (has_display_peak
                  ? " (capture display metadata)"
                  : " (1000-nit fallback)");
        }
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
    reset_hdr_pre_encode_transform() {
      if (!hdr_pre_encode || hdr_pre_encode.params.nominalPeakNits <= 0.0f ||
          (std::abs(hdr_pre_encode.params.sdrBandGain - 1.0f) < 0.01f &&
           std::abs(hdr_pre_encode.params.sdrBandTopScrgb) < 0.01f)) {
        return;
      }

      HdrPreEncodeParams params = hdr_pre_encode.params;
      params.sdrBandGain = 1.0f;
      params.sdrBandTopScrgb = 0.0f;
      auto next_buffer = make_buffer(device.get(), params);
      if (!next_buffer) {
        BOOST_LOG(warning) << "Failed to reset HDR pre-encode transform; retaining previous value"sv;
        return;
      }
      hdr_pre_encode.constantBuffer = std::move(next_buffer);
      hdr_pre_encode.params = params;
    }

    void
    set_client_sdr_white(float nits) {
      hdr_pre_encode.clientSdrWhiteNits = std::isfinite(nits) && nits > 0.0f ? nits : 0.0f;
      if (hdr_pre_encode.clientSdrWhiteNits <= 0.0f) {
        reset_hdr_pre_encode_transform();
      }
    }

    // Re-anchor SDR-referenced content to the client's SDR reference white.
    // Called per frame before PQ/HLG conversion; cheap float compare,
    // the constant buffer is only rebuilt when the gain actually changes.
    void
    update_hdr_pre_encode_transform() {
      if (hdr_pre_encode.clientSdrWhiteNits <= 0.0f) {
        reset_hdr_pre_encode_transform();
        return;
      }
      if (!hdr_pre_encode || hdr_pre_encode.params.nominalPeakNits <= 0.0f) {
        return;
      }
      auto vram_display = std::dynamic_pointer_cast<platf::dxgi::display_vram_t>(display);
      if (!vram_display) {
        return;
      }
      const auto windows_white = vram_display->capture_sdr_white_nits();
      if (!windows_white || *windows_white < 50.0f) {
        return;
      }

      const float gain = std::clamp(hdr_pre_encode.clientSdrWhiteNits / *windows_white, 0.5f, 2.5f);
      const float band_top = *windows_white / 80.0f;
      if (std::abs(gain - hdr_pre_encode.params.sdrBandGain) < 0.01f &&
          std::abs(band_top - hdr_pre_encode.params.sdrBandTopScrgb) < 0.01f) {
        return;
      }

      HdrPreEncodeParams params = hdr_pre_encode.params;
      params.sdrBandGain = gain;
      params.sdrBandTopScrgb = band_top;
      auto next_buffer = make_buffer(device.get(), params);
      if (!next_buffer) {
        BOOST_LOG(warning) << "Failed to update HDR SDR band gain; retaining previous value"sv;
        return;
      }
      hdr_pre_encode.constantBuffer = std::move(next_buffer);
      hdr_pre_encode.params = params;
      BOOST_LOG(info) << "SDR band gain: client " << hdr_pre_encode.clientSdrWhiteNits
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

      if (configure_hdr_pre_encode(use_pq_shader, use_hlg_shader, is_probe) != 0) {
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
        init_compute_path(
          out_width,
          out_height,
          active_w,
          active_h,
          active_off_x,
          active_off_y,
          colorspace,
          is_probe);
      }

      // D3D12 analysis needs the converter's shared cell-statistics snapshot.
      // A skipped compute path must not silently satisfy an explicit strict
      // request with D3D11 analysis. Preserve any more specific failure reason.
      if (!is_probe && hdr_analysis_enabled && !d3d12_hdr_analysis) {
        if (auto vram = std::dynamic_pointer_cast<display_vram_t>(display);
            vram && vram->video_backend_selection &&
            vram->video_backend_selection->requested == video_backend::windows_video_backend_e::d3d12 &&
            vram->video_backend_selection->fallback == video_backend::fallback_reason_e::none) {
          vram->disable_d3d12_analysis("hdr_snapshot_path_unavailable", E_NOTIMPL,
            video_backend::fallback_reason_e::analysis_path_unavailable);
        }
      }
      if (!video_backend_available()) return -1;
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

      if (this->display->pre_encode_filter != pre_encode_filter_e::none) {
        const bool hdr_output =
          format == DXGI_FORMAT_P010 || format == DXGI_FORMAT_Y410 || format == DXGI_FORMAT_R16_UINT;
        if (!hdr_output) {
          BOOST_LOG(error) << "Pre-encode HDR filter requires a 10-bit HDR encoder surface"sv;
          return -1;
        }
        const auto &contract = this->display->capture_contract;
        if (contract.required_domain != frame_domain_e::sdr_rec709 ||
            contract.preferred_encoding != pixel_encoding_class_e::unorm8 ||
            !contract.require_private_handoff) {
          BOOST_LOG(error) << "Pre-encode HDR filter requires a private SDR UNORM capture contract"sv;
          return -1;
        }
        pre_encode_filter = make_pre_encode_filter(
          this->display->pre_encode_filter,
          device.get(),
          device_ctx.get(),
          this->display->pre_encode_filter_backend_path,
          this->display->pre_encode_filter_config);
        if (!pre_encode_filter) {
          BOOST_LOG(error) << "Failed to create pre-encode filter"sv;
          return -1;
        }
      }

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

    bool
    prepare_filter_handoff(
      ID3D11Texture2D *source,
      const captured_frame_desc_t &semantic) {
      if (!source || semantic.domain != frame_domain_e::sdr_rec709 ||
          semantic.encoding != pixel_encoding_class_e::unorm8) {
        BOOST_LOG(error) << "Cannot detach unsupported pre-encode filter input"sv;
        return false;
      }

      D3D11_TEXTURE2D_DESC source_desc {};
      source->GetDesc(&source_desc);
      if (filter_handoff_texture &&
          filter_handoff_width == source_desc.Width &&
          filter_handoff_height == source_desc.Height &&
          filter_handoff_format == source_desc.Format &&
          filter_handoff_generation == semantic.source_generation) {
        return true;
      }

      filter_handoff_srv.reset();
      filter_handoff_texture.reset();
      source_desc.MipLevels = 1;
      source_desc.ArraySize = 1;
      source_desc.Usage = D3D11_USAGE_DEFAULT;
      source_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      source_desc.CPUAccessFlags = 0;
      source_desc.MiscFlags = 0;
      auto status = device->CreateTexture2D(&source_desc, nullptr, &filter_handoff_texture);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to create private filter handoff texture: "sv << util::log_hex(status);
        return false;
      }
      status = device->CreateShaderResourceView(
        filter_handoff_texture.get(), nullptr, &filter_handoff_srv);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to create private filter handoff SRV: "sv << util::log_hex(status);
        filter_handoff_texture.reset();
        return false;
      }
      filter_handoff_width = source_desc.Width;
      filter_handoff_height = source_desc.Height;
      filter_handoff_format = source_desc.Format;
      filter_handoff_generation = semantic.source_generation;
      return true;
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
      if ((gpu_timing_frame_counter % gpu_timing_sample_interval) != 0 ||
          gpu_timing_pending.size() >= vram_gpu_timing_max_pending) {
        return false;
      }

      if (!gpu_timing_reusable.empty()) {
        sample = std::move(gpu_timing_reusable.back());
        gpu_timing_reusable.pop_back();
        sample.m0.reset();
        sample.cs_used = sample.scratch_copy = sample.direct_uav = false;
        sample.p010 = sample.scaled = sample.borrowed_vdd = false;
      }
      else {
        sample.disjoint = make_query(D3D11_QUERY_TIMESTAMP_DISJOINT);
        sample.start = make_query(D3D11_QUERY_TIMESTAMP);
        sample.after_dispatch = make_query(D3D11_QUERY_TIMESTAMP);
        sample.before_copy = make_query(D3D11_QUERY_TIMESTAMP);
        sample.after_copy = make_query(D3D11_QUERY_TIMESTAMP);
        sample.end = make_query(D3D11_QUERY_TIMESTAMP);
        if (!sample.disjoint || !sample.start || !sample.after_dispatch ||
            !sample.before_copy || !sample.after_copy ||
            !sample.m0.initialize([&]() {
              return make_query(D3D11_QUERY_TIMESTAMP);
            }) ||
            !sample.end) {
          gpu_timing_disabled = true;
          return false;
        }
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
        constexpr UINT read_flags = D3D11_ASYNC_GETDATA_DONOTFLUSH;
        HRESULT status = device_ctx->GetData(sample.disjoint.get(), &disjoint, sizeof(disjoint), read_flags);
        if (status != S_OK) {
          break;
        }

        UINT64 start = 0;
        UINT64 after_dispatch = 0;
        UINT64 before_copy = 0;
        UINT64 after_copy = 0;
        telemetry::d3d11_stage_values_t m0_values {};
        UINT64 end = 0;
        bool ready =
          device_ctx->GetData(sample.start.get(), &start, sizeof(start), read_flags) == S_OK &&
          device_ctx->GetData(sample.after_dispatch.get(), &after_dispatch, sizeof(after_dispatch), read_flags) == S_OK &&
          device_ctx->GetData(sample.before_copy.get(), &before_copy, sizeof(before_copy), read_flags) == S_OK &&
          device_ctx->GetData(sample.after_copy.get(), &after_copy, sizeof(after_copy), read_flags) == S_OK &&
          device_ctx->GetData(sample.end.get(), &end, sizeof(end), read_flags) == S_OK &&
          sample.m0.read(device_ctx, m0_values, read_flags);
        if (!ready) {
          break;
        }

        if (!disjoint.Disjoint && disjoint.Frequency != 0) {
          gpu_timing_stats.total.add(gpu_delta_ms(start, end, disjoint.Frequency));
          sample.m0.accumulate(
            gpu_timing_stats.m0,
            m0_values,
            disjoint.Frequency,
            start,
            before_copy,
            after_copy,
            sample.scratch_copy);
          if (vram_timing_raw_enabled) {
            telemetry::m0_pipeline_metrics_t one;
            sample.m0.accumulate(one, m0_values, disjoint.Frequency, start, before_copy, after_copy, sample.scratch_copy);
            BOOST_LOG(info) << "[vram] gpu_sample source_frame=" << sample.source_frame
                            << " timing_scope=d3d11_queue_only total_gpu_ms="
                            << gpu_delta_ms(start, end, disjoint.Frequency) << telemetry::m0_metric_fields(one);
          }
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

        gpu_timing_reusable.emplace_back(std::move(sample));
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
      if (gpu_timing_stats.total.empty() &&
          gpu_timing_stats.disjoint_samples == 0 &&
          gpu_timing_stats.m0.empty()) {
        return;
      }

      const auto total = gpu_timing_stats.total.summary();
      const auto dispatch = gpu_timing_stats.dispatch.summary();
      const auto unbind = gpu_timing_stats.unbind.summary();
      const auto scratch_copy = gpu_timing_stats.scratch_copy.summary();
      BOOST_LOG(info) << "[vram] gpu_metrics timing_scope=d3d11_queue_only samples="sv << total.samples
                      << " cs="sv << gpu_timing_stats.cs_samples
                      << " draw="sv << gpu_timing_stats.draw_samples
                      << " direct_uav="sv << gpu_timing_stats.direct_uav_samples
                      << " scratch="sv << gpu_timing_stats.scratch_samples
                      << " p010="sv << gpu_timing_stats.p010_samples
                      << " scaled="sv << gpu_timing_stats.scaled_samples
                      << " borrowed_vdd="sv << gpu_timing_stats.borrowed_vdd_samples
                      << " pending="sv << gpu_timing_pending.size()
                      << " disjoint="sv << gpu_timing_stats.disjoint_samples
                      << telemetry::metric_fields("total_gpu_ms", total)
                      << telemetry::metric_fields("dispatch_gpu_ms", dispatch)
                      << telemetry::metric_fields("unbind_gpu_ms", unbind)
                      << telemetry::metric_fields("scratch_copy_gpu_ms", scratch_copy)
                      << telemetry::m0_metric_fields(gpu_timing_stats.m0);
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
      if (cpu_acquire_timing.empty() && cpu_submit_timing.empty()) {
        return;
      }

      const auto acquire = cpu_acquire_timing.summary();
      const auto submit = cpu_submit_timing.summary();
      const auto result_age = analysis_result_age_timing.summary();
      BOOST_LOG(info) << "[vram] cpu_metrics"sv
                      << telemetry::m0_cpu_metric_fields(
                           acquire,
                           submit,
                           result_age,
                           analysis_skipped_busy);
      cpu_acquire_timing.reset();
      cpu_submit_timing.reset();
      analysis_result_age_timing.reset();
      analysis_skipped_busy = 0;
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
    update_synthetic_hdr_runtime_status(
      bool processed_frame,
      std::string_view frame_failure = {}) {
      if (!pre_encode_filter) {
        runtime_status.synthetic_hdr_backend = "none";
        runtime_status.synthetic_hdr_state = "disabled";
        runtime_status.synthetic_hdr_failure_reason.clear();
        return;
      }

      const std::string backend { pre_encode_filter->backend_name() };
      const std::string state = !frame_failure.empty() || pre_encode_filter->degraded()
                                  ? "degraded"
                                  : processed_frame ? "active" : "warming_up";
      const std::string reason = frame_failure.empty()
                                   ? std::string { pre_encode_filter->failure_reason() }
                                   : std::string { frame_failure };
      if (runtime_status.synthetic_hdr_backend == backend &&
          runtime_status.synthetic_hdr_state == state &&
          runtime_status.synthetic_hdr_failure_reason == reason) {
        return;
      }
      runtime_status.synthetic_hdr_backend = backend;
      runtime_status.synthetic_hdr_state = state;
      runtime_status.synthetic_hdr_failure_reason = reason;
      ::video::update_hdr_pipeline_status(runtime_status_id, runtime_status);
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
      update_synthetic_hdr_runtime_status(false);

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
    HdrPreEncodeState hdr_pre_encode;

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

    std::unique_ptr<pre_encode_filter_t> pre_encode_filter;
    texture2d_t filter_handoff_texture;
    shader_res_t filter_handoff_srv;
    std::uint32_t filter_handoff_width = 0;
    std::uint32_t filter_handoff_height = 0;
    DXGI_FORMAT filter_handoff_format = DXGI_FORMAT_UNKNOWN;
    std::uint64_t filter_handoff_generation = 0;

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
    std::vector<gpu_timing_sample_t> gpu_timing_reusable;
    gpu_timing_stats_t gpu_timing_stats;
    timing_bucket_t cpu_acquire_timing;
    timing_bucket_t cpu_submit_timing;
    timing_bucket_t analysis_result_age_timing;
    std::chrono::steady_clock::time_point gpu_timing_last_log {};
    std::chrono::steady_clock::time_point cpu_timing_last_log {};
    uint64_t gpu_timing_frame_counter = 0;
    uint64_t video_frame_counter = 0;
    uint64_t analysis_skipped_busy = 0;
    bool vram_timing_enabled = env_flag_enabled("SUNSHINE_VRAM_TIMING");
    bool vram_timing_raw_enabled = env_flag_enabled("SUNSHINE_VRAM_TIMING_RAW");
    uint64_t gpu_timing_sample_interval = timing_sample_interval();
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
    buf_t hdr_staging_buf;                 // One non-blocking CPU readback; new analysis waits while pending
    buf_t hdr_analysis_cbuf;               // Constant buffer for pass 1 (analysis resolution)
    buf_t hdr_analysis_snapshot_cbuf;      // Shared converter/pass 1 params for the snapshot
    buf_t hdr_reduce_cbuf;                 // Constant buffer for pass 2 (numGroups)
    uint32_t hdr_analysis_width = 0;       // Analysis grid width (downsampled from source)
    uint32_t hdr_analysis_height = 0;      // Analysis grid height (downsampled from source)
    uint32_t hdr_num_groups = 0;           // Number of thread groups dispatched in pass 1
    uint64_t hdr_analysis_frame_index = 0; // Used to downsample analysis frequency
    uint64_t hdr_analysis_pending_source_frame = 0;
    std::optional<uint64_t> hdr_analysis_last_completed_frame;
    uint64_t hdr_analysis_sample_sequence = 0; // Counts completed, independent GPU samples
    bool hdr_analysis_pending = false;     // Prevents overwriting a readback the GPU has not completed
    bool hdr_analysis_ready = false;       // Whether the analyzer's GPU resources were created
    bool hdr_analysis_enabled = false;     // Whether analysis runs: resources exist and the stream can carry metadata
    ::video::hdr_metadata::formats_t hdr_metadata_formats;  // Dynamic metadata formats this stream may carry
    bool hdr_analysis_snapshot_enabled = false; // P010 converter fills the private analysis texture
    float hdr_analysis_max_nits = 10000.0f; // Clamp metadata to the encoded transfer-function range
    std::string hdr_analysis_failure_reason;
    std::unique_ptr<d3d12::hdr_analysis_t> d3d12_hdr_analysis;

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
    using FinalResult = hdr_analysis::result_t;

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

      // --- Staging ring for asynchronous CPU readback ---
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
     * @brief Prepare the common analysis contract from either available source.
     */
    HdrAnalysisSource
    prepare_hdr_analysis_source(
      bool snapshot_written,
      ID3D11Texture2D *encoder_texture,
      gpu_timing_sample_t *timing) {
      if (snapshot_written) {
        return {
          hdr_analysis_snapshot_srv.get(),
          hdr_analysis_pq_srv.get(),
          hdr_analysis_snapshot_cbuf.get(),
        };
      }

      // Pixel-shader fallback: preserve the source outside the encoder keyed
      // mutex, then let the common pass-1 analyzer apply HdrPreEncodeTransform.
      if (!hdr_analysis_input_tex || !encoder_texture) {
        return {};
      }
      D3D11_TEXTURE2D_DESC analysis_desc {};
      D3D11_TEXTURE2D_DESC encoder_desc {};
      hdr_analysis_input_tex->GetDesc(&analysis_desc);
      encoder_texture->GetDesc(&encoder_desc);
      if (analysis_desc.Width != encoder_desc.Width ||
          analysis_desc.Height != encoder_desc.Height ||
          analysis_desc.MipLevels != encoder_desc.MipLevels ||
          analysis_desc.ArraySize != encoder_desc.ArraySize ||
          analysis_desc.Format != encoder_desc.Format ||
          analysis_desc.SampleDesc.Count != encoder_desc.SampleDesc.Count ||
          analysis_desc.SampleDesc.Quality != encoder_desc.SampleDesc.Quality) {
        // WGC window capture may change size independently of the display-sized
        // analysis resources. Dropping this sample is safer than copying
        // incompatible resources and reusing stale luminance metadata.
        return {};
      }
      if (timing) {
        timing->m0.begin_capture_copy(device_ctx);
      }
      device_ctx->CopyResource(hdr_analysis_input_tex.get(), encoder_texture);
      if (timing) {
        timing->m0.end_capture_copy(device_ctx);
      }
      return {
        hdr_analysis_input_srv.get(),
        nullptr,
        hdr_analysis_cbuf.get(),
      };
    }

    /**
     * @brief Dispatch the two-pass luminance analysis for the current frame.
     * Pass 1: Per-tile analysis — reads scRGB texture, writes per-group results
     * Pass 2: Global reduction — reads per-group results, writes 1 final result
     * Then copies final result to staging for async CPU readback next frame.
     * @param source Unified full-frame or snapshot analysis input.
     */
    void
    dispatch_hdr_analysis(
      const HdrAnalysisSource &source,
      uint64_t source_frame_index,
      gpu_timing_sample_t *timing) {
      if (!hdr_analysis_enabled || !source) return;
      if (hdr_analysis_pending) {
        // The GPU is already behind this analysis cadence. Drop the new sample
        // instead of queuing progressively older metadata or blocking capture.
        return;
      }

      if (timing) {
        timing->m0.begin_analysis(device_ctx);
      }

      // Unbind render targets to avoid resource hazard (SRV vs RTV conflict)
      ID3D11RenderTargetView *null_rtv = nullptr;
      device_ctx->OMSetRenderTargets(1, &null_rtv, nullptr);

      // The global histogram accumulates across the whole frame, so it must start at zero.
      const UINT hist_clear[4] = { 0, 0, 0, 0 };
      device_ctx->ClearUnorderedAccessViewUint(hdr_global_histogram_uav.get(), hist_clear);

      // ===== Pass 1: Per-tile analysis =====
      device_ctx->CSSetShader(hdr_pass1_cs.get(), nullptr, 0);
      // t1 stays unbound on the full-frame fallback, which computes the PQ sum itself.
      ID3D11ShaderResourceView *pass1_srvs[] = { source.statistics, source.pqAverage };
      device_ctx->CSSetShaderResources(0, 2, pass1_srvs);
      ID3D11UnorderedAccessView *pass1_uavs[] = { hdr_group_results_uav.get(), hdr_global_histogram_uav.get() };
      device_ctx->CSSetUnorderedAccessViews(0, 2, pass1_uavs, nullptr);
      ID3D11Buffer *analysis_params = source.parameters;
      device_ctx->CSSetConstantBuffers(0, 1, &analysis_params);
      if (hdr_pre_encode) {
        ID3D11Buffer *pre_encode_cbuf = hdr_pre_encode.constantBuffer.get();
        device_ctx->CSSetConstantBuffers(3, 1, &pre_encode_cbuf);
      }

      uint32_t groups_x = (hdr_analysis_width + 15) / 16;
      uint32_t groups_y = (hdr_analysis_height + 15) / 16;
      device_ctx->Dispatch(groups_x, groups_y, 1);
      if (timing) {
        timing->m0.end_analysis_pass1(device_ctx);
      }

      // Unbind pass 1 resources
      ID3D11ShaderResourceView *null_srv = nullptr;
      ID3D11ShaderResourceView *null_srvs[2] = { nullptr, nullptr };
      ID3D11UnorderedAccessView *null_uavs[2] = { nullptr, nullptr };
      device_ctx->CSSetShaderResources(0, 2, null_srvs);
      device_ctx->CSSetUnorderedAccessViews(0, 2, null_uavs, nullptr);
      ID3D11Buffer *null_pre_encode_cbuf = nullptr;
      device_ctx->CSSetConstantBuffers(3, 1, &null_pre_encode_cbuf);

      // ===== Pass 2: Global reduction =====
      device_ctx->CSSetShader(hdr_pass2_cs.get(), nullptr, 0);
      ID3D11ShaderResourceView *group_srv = hdr_group_results_srv.get();
      device_ctx->CSSetShaderResources(0, 1, &group_srv);
      ID3D11UnorderedAccessView *pass2_uavs[] = { hdr_final_result_uav.get(), hdr_global_histogram_uav.get() };
      device_ctx->CSSetUnorderedAccessViews(0, 2, pass2_uavs, nullptr);
      ID3D11Buffer *cbuf = hdr_reduce_cbuf.get();
      device_ctx->CSSetConstantBuffers(0, 1, &cbuf);

      device_ctx->Dispatch(1, 1, 1);  // Single group of 256 threads
      if (timing) {
        timing->m0.end_analysis_pass2(device_ctx);
      }

      // Unbind all CS resources
      device_ctx->CSSetShaderResources(0, 1, &null_srv);
      device_ctx->CSSetUnorderedAccessViews(0, 2, null_uavs, nullptr);
      ID3D11Buffer *null_cb = nullptr;
      device_ctx->CSSetConstantBuffers(0, 1, &null_cb);
      device_ctx->CSSetShader(nullptr, nullptr, 0);

      device_ctx->CopyResource(hdr_staging_buf.get(), hdr_final_result_buf.get());
      if (timing) {
        timing->m0.end_analysis_readback(device_ctx);
      }

      hdr_analysis_pending_source_frame = source_frame_index;
      hdr_analysis_pending = true;
    }

    /**
     * @brief Read HDR analysis results from the staging buffer (previous frame).
     * GPU has already reduced all groups to one FinalResult — CPU just reads it
     * and computes PQ-domain percentiles from the histogram.
     */
    void
    read_hdr_analysis_results(uint64_t current_frame_index) {
      if (!hdr_analysis_pending) {
        return;
      }
      D3D11_MAPPED_SUBRESOURCE mapped = {};
      HRESULT status = device_ctx->Map(
        hdr_staging_buf.get(), 0,
        D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);

      if (status == DXGI_ERROR_WAS_STILL_DRAWING) {
        if (vram_timing_enabled) {
          ++gpu_timing_stats.m0.analysis_readback_not_ready;
        }
        // GPU hasn't finished yet — skip this readback, try next frame
        return;
      }

      if (FAILED(status)) {
        BOOST_LOG(debug) << "HDR staging Map failed: " << util::log_hex(status);
        hdr_analysis_pending = false;
        return;
      }

      auto *result = reinterpret_cast<const FinalResult *>(mapped.pData);

      publish_hdr_analysis_result(result, hdr_analysis_pending_source_frame, current_frame_index);

      device_ctx->Unmap(hdr_staging_buf.get(), 0);
      hdr_analysis_pending = false;
    }

    // Both APIs share decoding; publication order remains owned by the session.
    void
    publish_hdr_analysis_result(const FinalResult *result, uint64_t source_frame_index, uint64_t current_frame_index) {
      if (result->pixel_count == 0 ||
          (hdr_analysis_last_completed_frame && source_frame_index <= *hdr_analysis_last_completed_frame)) {
        return;
      }
      hdr_luminance_stats_out = hdr_analysis::decode_result(
        *result, hdr_analysis_max_nits, ++hdr_analysis_sample_sequence);
      hdr_luminance_stats_out.source_frame = source_frame_index;
      hdr_analysis_last_completed_frame = std::min(current_frame_index, source_frame_index);
    }

    void
    read_d3d12_hdr_analysis_results(uint64_t current_frame_index) {
      if (!d3d12_hdr_analysis || !d3d12_hdr_analysis->available()) {
        return;
      }
      const auto completed = d3d12_hdr_analysis->poll();
      if (!completed) {
        if (!d3d12_hdr_analysis->available()) {
          if (auto display_vram =
                std::dynamic_pointer_cast<display_vram_t>(
                  display)) {
            display_vram->disable_d3d12_analysis(
              d3d12_hdr_analysis->failure_stage(),
              d3d12_hdr_analysis->failure_hresult());
          }
        }
        return;
      }
      if (completed->timing) {
        const auto &timing = *completed->timing;
        gpu_timing_stats.m0.analysis_pass1.add(timing.pass1_ms);
        gpu_timing_stats.m0.analysis_pass2.add(timing.pass2_ms);
        gpu_timing_stats.m0.analysis_readback_copy.add(timing.readback_ms);
        if (vram_timing_raw_enabled) {
          BOOST_LOG(info) << "[vram] d3d12_gpu_sample source_frame=" << completed->source_frame
                          << " analysis_gpu_total_ms=" << timing.gpu_total_ms
                          << " analysis_pass1_gpu_ms=" << timing.pass1_ms
                          << " analysis_pass2_gpu_ms=" << timing.pass2_ms
                          << " analysis_readback_copy_gpu_ms=" << timing.readback_ms
                          << " submit_to_gpu_start_ms=" << timing.submit_to_start_ms.value_or(-1)
                          << " submit_to_poll_ms=" << timing.poll_latency_ms;
        }
      }
      publish_hdr_analysis_result(&completed->result, completed->source_frame, current_frame_index);
    }

    // ===== Compute-shader RGB->P010 fast path (Phase 1) =====
    // Allocates a P010 scratch texture with UAV bind, creates plane UAVs and a layout cbuffer.
    // Probes runtime support; if anything fails, leaves cs_path_active = false (PS path stays active).
    // Called from init_output() after standard setup. Non-fatal on failure.
    void
    init_compute_path(int out_width, int out_height,
                      int active_w, int active_h,
                      int active_offset_x, int active_offset_y,
                      const ::video::sunshine_colorspace_t &colorspace,
                      bool is_probe) {
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
      d3d12_hdr_analysis.reset();
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

          auto display_vram =
            std::dynamic_pointer_cast<display_vram_t>(display);
          if (display_vram) {
            d3d12_hdr_analysis =
              display_vram->make_d3d12_hdr_analysis(
              device.get(),
              device_ctx.get(),
              hdr_analysis_width,
              hdr_analysis_height,
              static_cast<uint32_t>(active_w),
              static_cast<uint32_t>(active_h),
              hdr_analysis_max_nits,
              is_probe);
          }
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
      gpu_timing_sample_t *timing,
      ID3D11UnorderedAccessView *snapshot_uav_override = nullptr,
      ID3D11UnorderedAccessView *pq_uav_override = nullptr) {
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
        write_hdr_analysis_snapshot ?
          (snapshot_uav_override ? snapshot_uav_override : hdr_analysis_snapshot_uav.get()) :
          nullptr,
        write_hdr_analysis_snapshot ?
          (pq_uav_override ? pq_uav_override : hdr_analysis_pq_uav.get()) :
          nullptr,
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
      if (hdr_pre_encode) {
        ID3D11Buffer *pre_encode_cbuf = hdr_pre_encode.constantBuffer.get();
        device_ctx->CSSetConstantBuffers(3, 1, &pre_encode_cbuf);
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
      ID3D11Buffer *null_pre_encode_cbuf = nullptr;
      device_ctx->CSSetConstantBuffers(3, 1, &null_pre_encode_cbuf);
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
      auto nvenc_config = config::video.nv;
      // The block-linear array input is experimental: it has stalled encoder
      // probing on some drivers and a wedged probe delays every stream start.
      // Probe the known-good pitch-linear path; only real sessions opt in.
      nvenc_config.cuda_array_input = nvenc_config.cuda_array_input && !is_probe;
      if (!nvenc_d3d->create_encoder(nvenc_config, client_config, colorspace, buffer_format)) return false;

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
    if (!prepare_video_backend()) return nullptr;
    auto device = std::make_unique<d3d_avcodec_encode_device_t>();
    if (device->init(shared_from_this(), adapter.get(), pix_fmt) != 0) {
      return nullptr;
    }
    return device;
  }

  std::unique_ptr<nvenc_encode_device_t>
  display_vram_t::make_nvenc_encode_device(pix_fmt_e pix_fmt) {
    if (!prepare_video_backend()) return nullptr;
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
    if (!prepare_video_backend()) return nullptr;
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

}  // namespace platf::dxgi
