/**
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 Foundation Sunshine contributors
 *
 * @file src/platform/windows/image_enhancement/dlss_nr/dlssnr_filter.cpp
 * @brief NVIDIA DLSS NR adapter for the neutral pre-encode filter contract.
 */
#include "dlssnr_filter.h"
#include "../../pre_encode_filter_helpers.h"
#include "adapter_loader.h"
#include "hdr_filter.h"

#include <utility>

#include <boost/thread/lock_guard.hpp>
#include <boost/thread/mutex.hpp>

namespace platf::dxgi::image_enhancement::dlss_nr {
  namespace {
    using namespace filter_detail;
    boost::mutex adapter_mutex;

    std::string_view
    dlssnr_failure_reason(foundation_dlssnr_status_e status, bool during_create) {
      switch (status) {
        case FOUNDATION_DLSSNR_STATUS_INVALID_ARGUMENT:
          return during_create ? "backend_create_invalid_argument" : "backend_process_invalid_argument";
        case FOUNDATION_DLSSNR_STATUS_UNSUPPORTED:
          return during_create ? "backend_create_unsupported" : "backend_process_unsupported";
        case FOUNDATION_DLSSNR_STATUS_RUNTIME_UNAVAILABLE:
          return during_create ? "backend_create_runtime_unavailable" : "backend_process_runtime_unavailable";
        case FOUNDATION_DLSSNR_STATUS_DEVICE_LOST:
          return during_create ? "backend_create_device_lost" : "backend_process_device_lost";
        case FOUNDATION_DLSSNR_STATUS_INTERNAL_ERROR:
          return during_create ? "backend_create_internal_error" : "backend_process_internal_error";
        case FOUNDATION_DLSSNR_STATUS_OK:
          break;
      }
      return during_create ? "backend_create_unknown_error" : "backend_process_unknown_error";
    }

    class external_neural_enhancement_filter_t final: public pre_encode_filter_t {
    public:
      external_neural_enhancement_filter_t(
        ID3D11Device *device,
        ID3D11DeviceContext *device_context,
        dlssnr_adapter_loader_t loader,
        pre_encode_filter_config_t config):
          device_ { device },
          device_context_ { device_context },
          loader_ { std::move(loader) },
          config_ { config } {}

      ~external_neural_enhancement_filter_t() override {
        destroy_instance();
      }

      filter_result_t
      process(const gpu_frame_view_t &input) override {
        if (const auto reason = validate_sdr_input(input); !reason.empty()) {
          return { .status = filter_status_e::failed, .frame = {}, .reason = reason };
        }
        if (!ensure_output_and_instance(input.width, input.height)) {
          return { .status = filter_status_e::failed, .frame = {}, .reason = initialization_failure_ };
        }

        foundation_dlssnr_status_e status;
        {
          boost::lock_guard lock { adapter_mutex };
          status = loader_.api()->process(
            instance_,
            device_context_,
            input.texture,
            output_texture_.get());
        }
        if (status != FOUNDATION_DLSSNR_STATUS_OK) {
          return {
            .status = filter_status_e::failed,
            .frame = {},
            .reason = dlssnr_failure_reason(status, false),
          };
        }
        return make_sdr_result(input, output_texture_.get(), output_srv_.get());
      }

      void
      flush() override {
        destroy_instance();
      }

      std::string_view
      backend_name() const override {
        return "alkaidlab.nvidia_dlssnr";
      }

    private:
      bool
      ensure_output_and_instance(std::uint32_t width, std::uint32_t height) {
        if (instance_ && output_texture_ && width_ == width && height_ == height) {
          return true;
        }
        initialization_failure_ = "backend_initialization_failed";
        destroy_instance();
        output_srv_.reset();
        output_texture_.reset();

        // The adapter writes through its own cross-device mirror and copies
        // into this texture on the immediate context, so a plain SRV-capable
        // SDR surface is sufficient here.
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ID3D11Texture2D *texture_raw = nullptr;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, &texture_raw))) {
          initialization_failure_ = "backend_output_allocation_failed";
          return false;
        }
        output_texture_.reset(texture_raw);
        ID3D11ShaderResourceView *srv_raw = nullptr;
        if (FAILED(device_->CreateShaderResourceView(output_texture_.get(), nullptr, &srv_raw))) {
          initialization_failure_ = "backend_output_view_creation_failed";
          output_texture_.reset();
          return false;
        }
        output_srv_.reset(srv_raw);

        foundation_dlssnr_config_t config {
          .struct_size = sizeof(foundation_dlssnr_config_t),
          .width = width,
          .height = height,
          .intensity = config_.nr_intensity,
          .local_tone_strength = config_.nr_local_tone_strength,
          .local_structure_strength = config_.nr_local_structure_strength,
          .skin_structure_strength = config_.nr_skin_structure_strength,
          .style = config_.nr_style,
          .motion_mode = config_.nr_motion_quality > 0 ?
                           FOUNDATION_DLSSNR_MOTION_OPTICAL_FLOW :
                           FOUNDATION_DLSSNR_MOTION_ZERO,
          .motion_quality = config_.nr_motion_quality,
          .auto_mask = config_.nr_auto_mask ? std::uint8_t { 1 } : std::uint8_t { 0 },
          .ui_correction = config_.nr_ui_correction ? std::uint8_t { 1 } : std::uint8_t { 0 },
          .runtime_directory = loader_.runtime_directory(),
        };
        foundation_dlssnr_status_e status;
        {
          boost::lock_guard lock { adapter_mutex };
          status = loader_.api()->create(device_, &config, &instance_);
        }
        if (status != FOUNDATION_DLSSNR_STATUS_OK || !instance_) {
          initialization_failure_ = status == FOUNDATION_DLSSNR_STATUS_OK ? "backend_create_missing_instance" : dlssnr_failure_reason(status, true);
          instance_ = nullptr;
          output_srv_.reset();
          output_texture_.reset();
          return false;
        }
        width_ = width;
        height_ = height;
        initialization_failure_ = {};
        return true;
      }

      void
      destroy_instance() {
        if (instance_) {
          boost::lock_guard lock { adapter_mutex };
          loader_.api()->destroy(instance_);
          instance_ = nullptr;
        }
        width_ = 0;
        height_ = 0;
      }

      ID3D11Device *device_;
      ID3D11DeviceContext *device_context_;
      dlssnr_adapter_loader_t loader_;
      pre_encode_filter_config_t config_;
      void *instance_ = nullptr;
      std::string_view initialization_failure_ { "backend_initialization_failed" };
      com_ptr_t<ID3D11Texture2D> output_texture_;
      com_ptr_t<ID3D11ShaderResourceView> output_srv_;
      std::uint32_t width_ = 0;
      std::uint32_t height_ = 0;
    };

  }  // namespace
  std::unique_ptr<pre_encode_filter_t>
  make_filter(ID3D11Device *device, ID3D11DeviceContext *context,
    const std::filesystem::path &path, const pre_encode_filter_config_t &config,
    std::string_view runtime_digest, std::string &error) {
    dlssnr_adapter_loader_t loader;
    const auto adapter_path = path;
    const auto runtime_path = path.parent_path() / RUNTIME_FILENAME;
    const auto pinned_digest =
      runtime_digest.empty() ?
        std::nullopt :
        std::optional<std::string_view> { runtime_digest };
    if (!loader.load(adapter_path, runtime_path, pinned_digest)) {
      error = loader.error();
      return {};
    }
    return make_hdr_compatible_filter(device, context, std::make_unique<external_neural_enhancement_filter_t>(device, context, std::move(loader), config), config.nr_scale_percent);
  }
}  // namespace platf::dxgi::image_enhancement::dlss_nr
