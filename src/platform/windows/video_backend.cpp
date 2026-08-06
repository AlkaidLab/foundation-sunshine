/**
 * @file src/platform/windows/video_backend.cpp
 * @brief Windows video pipeline backend selection integration.
 */

#include "video_backend.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>

#include "d3d12/d3d12_hdr_analysis.h"
#include "display.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/utility.h"

namespace platf::dxgi {
  namespace {
    constexpr bool d3d12_stage_available = false;

    bool
    env_flag_enabled(const char *name) {
      const char *raw_value = std::getenv(name);
      if (!raw_value || !*raw_value) {
        return false;
      }

      std::string value { raw_value };
      std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) {
          return static_cast<char>(std::tolower(c));
        });
      return value == "1" || value == "true" || value == "on" || value == "yes";
    }

    std::optional<std::string_view>
    backend_environment_override() {
      const char *value = std::getenv("SUNSHINE_WINDOWS_VIDEO_BACKEND");
      if (!value || !*value) {
        return std::nullopt;
      }
      return std::string_view { value };
    }

    std::string
    adapter_luid_string(IDXGIAdapter1 *adapter) {
      DXGI_ADAPTER_DESC1 desc {};
      if (!adapter || FAILED(adapter->GetDesc1(&desc))) {
        return "unknown";
      }
      return std::to_string(static_cast<std::uint32_t>(desc.AdapterLuid.HighPart)) +
             ":" + std::to_string(desc.AdapterLuid.LowPart);
    }
  }  // namespace

  bool
  display_vram_t::prepare_video_backend() {
    if (!video_backend_selection) {
      video_backend_selection = video_backend::resolve(
        config::video.windows_video_backend,
        backend_environment_override(),
        env_flag_enabled("SUNSHINE_WINDOWS_VIDEO_BACKEND_STRICT"),
        d3d12_stage_available);

      if (video_backend_selection->invalid_value) {
        static std::once_flag invalid_value_warning;
        std::call_once(invalid_value_warning, []() {
          BOOST_LOG(warning) << "Invalid SUNSHINE_WINDOWS_VIDEO_BACKEND value; "
                                "valid options are: auto, d3d11, d3d12. Defaulting to 'auto'";
        });
      }

      if (video_backend_selection->requested ==
          video_backend::windows_video_backend_e::d3d12) {
        d3d12_video_device = std::make_unique<d3d12::device_t>();
        const auto init_result = d3d12_video_device->initialize(
          adapter.get(),
          device.get(),
          device_ctx.get());
        if (!init_result.success) {
          video_backend_selection->fallback = init_result.reason;
          video_backend_hresult = init_result.hresult;
          video_backend_stage = init_result.stage;
          d3d12_video_device.reset();
        }
        else {
          video_backend_stage = "compute_stage_unavailable";
          const auto &capabilities = d3d12_video_device->capabilities();
          BOOST_LOG(debug) << "[video_backend] d3d12_base=ready slots="
                           << d3d12::resource_ring_t::slot_count
                           << " shared_tier="
                           << static_cast<unsigned>(
                                capabilities.shared_resource_tier)
                           << " topology_a_nv12="
                           << (capabilities.nv12_encoder_surface ? "yes" : "no")
                           << " topology_a_p010="
                           << (capabilities.p010_encoder_surface ? "yes" : "no")
                           << " topology_b_rgba16f="
                           << (capabilities.rgba16f_bridge ? "yes" : "no");
        }
      }
    }

    const auto &selection = *video_backend_selection;
    if (!selection.pipeline_available() && !video_backend_selection_logged) {
      BOOST_LOG(error) << "[video_backend] strict_failure requested=d3d12"
                          " stage="
                       << video_backend_stage
                       << " reason=" << video_backend::to_string(selection.fallback)
                       << " hresult=0x" << util::hex(video_backend_hresult).to_string_view();
      video_backend_selection_logged = true;
    }
    return selection.pipeline_available();
  }

  std::unique_ptr<d3d12::hdr_analysis_t>
  display_vram_t::make_d3d12_hdr_analysis(
    ID3D11Device *d3d11_device,
    ID3D11DeviceContext *d3d11_context,
    std::uint32_t analysis_width,
    std::uint32_t analysis_height,
    std::uint32_t source_width,
    std::uint32_t source_height,
    float max_analysis_nits,
    bool is_probe) {
    if (is_probe || !d3d12_video_device) {
      return nullptr;
    }

    auto analyzer = std::make_unique<d3d12::hdr_analysis_t>();
    const auto current_generation =
      d3d12_video_device->resource_ring().generation();
    const auto next_generation =
      d3d12_video_generation == 0 ?
        current_generation :
        current_generation + 1;
    const auto init_result = analyzer->initialize(
      *d3d12_video_device,
      d3d11_device,
      d3d11_context,
      analysis_width,
      analysis_height,
      source_width,
      source_height,
      max_analysis_nits,
      next_generation);
    if (!init_result.success) {
      video_backend_stage = init_result.stage;
      video_backend_hresult = init_result.hresult;
      BOOST_LOG(info) << "D3D12 HDR analysis unavailable at "
                      << init_result.stage << ": "
                      << util::log_hex(init_result.hresult)
                      << "; D3D11 analysis remains active";
      return nullptr;
    }

    d3d12_video_generation = next_generation;
    if (video_backend_selection) {
      video_backend_selection->effective =
        video_backend::effective_backend_e::hybrid;
      video_backend_selection->fallback =
        video_backend::fallback_reason_e::none;
    }
    video_backend_stage = "hdr_analysis_ready";
    video_backend_hresult = S_OK;
    BOOST_LOG(info)
      << "D3D12 HDR analysis enabled with asynchronous "
         "3-slot readback ring";
    return analyzer;
  }

  void
  display_vram_t::disable_d3d12_analysis(
    std::string_view stage,
    HRESULT hresult) {
    video_backend_stage = stage;
    video_backend_hresult = hresult;
    if (video_backend_selection &&
        video_backend_selection->effective ==
          video_backend::effective_backend_e::hybrid) {
      video_backend_selection->effective =
        video_backend::effective_backend_e::d3d11;
      video_backend_selection->fallback =
        video_backend::fallback_reason_e::runtime_fence_failed;
    }
    BOOST_LOG(warning)
      << "[video_backend] runtime_fallback from=d3d12_analysis"
         " to=d3d11_analysis stage="
      << stage << " hresult=0x"
      << util::hex(hresult).to_string_view();
  }

  void
  display_vram_t::report_video_backend_selection(std::string_view encoder_backend) {
    if (!video_backend_selection || video_backend_selection_logged) {
      return;
    }

    const auto &selection = *video_backend_selection;
    const auto d3d12_base =
      selection.requested != video_backend::windows_video_backend_e::d3d12 ?
        "not_requested" :
      d3d12_video_device ? "ready" :
                           "unavailable";
    const auto analysis_backend =
      selection.effective == video_backend::effective_backend_e::hybrid ?
        "d3d12" :
        "d3d11";
    BOOST_LOG(info) << "[video_backend] requested=" << video_backend::to_string(selection.requested)
                    << " effective=" << video_backend::to_string(selection.effective)
                    << " capture=d3d11 conversion=d3d11 analysis="
                    << analysis_backend
                    << " encoder=" << encoder_backend
                    << " adapter_luid=" << adapter_luid_string(adapter.get())
                    << " d3d12_base=" << d3d12_base
                    << " fallback=" << video_backend::to_string(selection.fallback)
                    << " strict=" << (selection.strict ? "true" : "false");
    video_backend_selection_logged = true;

    if (selection.requested == video_backend::windows_video_backend_e::d3d12 &&
        selection.fallback != video_backend::fallback_reason_e::none) {
      BOOST_LOG(warning) << "[video_backend] fallback from=d3d12 to=d3d11"
                            " stage="
                         << video_backend_stage
                         << " reason=" << video_backend::to_string(selection.fallback)
                         << " hresult=0x" << util::hex(video_backend_hresult).to_string_view();
    }
  }
}  // namespace platf::dxgi
