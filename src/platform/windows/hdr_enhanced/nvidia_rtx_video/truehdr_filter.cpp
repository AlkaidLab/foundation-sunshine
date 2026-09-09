/**
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 Foundation Sunshine contributors
 *
 * @file src/platform/windows/hdr_enhanced/nvidia_rtx_video/truehdr_filter.cpp
 * @brief NVIDIA TrueHDR adapter for the neutral pre-encode filter contract.
 */
#include "truehdr_filter.h"
#include "../../pre_encode_filter_helpers.h"
#include "adapter_loader.h"

#include <utility>

#include <boost/thread/lock_guard.hpp>
#include <boost/thread/mutex.hpp>

namespace platf::dxgi::hdr_enhanced::nvidia_rtx_video::truehdr {
  namespace {
    using namespace filter_detail;
    boost::mutex adapter_mutex;
    std::string_view
    truehdr_failure_reason(foundation_truehdr_status_e status, bool during_create) {
      switch (status) {
        case FOUNDATION_TRUEHDR_STATUS_INVALID_ARGUMENT:
          return during_create ? "backend_create_invalid_argument" : "backend_process_invalid_argument";
        case FOUNDATION_TRUEHDR_STATUS_UNSUPPORTED:
          return during_create ? "backend_create_unsupported" : "backend_process_unsupported";
        case FOUNDATION_TRUEHDR_STATUS_RUNTIME_UNAVAILABLE:
          return during_create ? "backend_create_runtime_unavailable" : "backend_process_runtime_unavailable";
        case FOUNDATION_TRUEHDR_STATUS_DEVICE_LOST:
          return during_create ? "backend_create_device_lost" : "backend_process_device_lost";
        case FOUNDATION_TRUEHDR_STATUS_INTERNAL_ERROR:
          return during_create ? "backend_create_internal_error" : "backend_process_internal_error";
        case FOUNDATION_TRUEHDR_STATUS_DEVELOPMENT_BUILD_EXPIRED:
          return during_create ? "backend_create_development_build_expired" : "backend_process_development_build_expired";
        case FOUNDATION_TRUEHDR_STATUS_OK:
          break;
      }
      return during_create ? "backend_create_unknown_error" : "backend_process_unknown_error";
    }

    class external_sdr_to_hdr_filter_t final: public pre_encode_filter_t {
    public:
      external_sdr_to_hdr_filter_t(
        ID3D11Device *device,
        ID3D11DeviceContext *device_context,
        adapter_loader_t loader,
        pre_encode_filter_config_t config):
          device_ { device },
          device_context_ { device_context },
          loader_ { std::move(loader) },
          config_ { config } {}

      ~external_sdr_to_hdr_filter_t() override {
        destroy_instance();
      }

      bool
      requires_detached_input() const override {
        return true;
      }

      filter_result_t
      process(const gpu_frame_view_t &input) override {
        if (const auto reason = validate_sdr_input(input); !reason.empty()) {
          return { .status = filter_status_e::failed, .frame = {}, .reason = reason };
        }
        if (!ensure_output_and_instance(input.width, input.height)) {
          return { .status = filter_status_e::failed, .frame = {}, .reason = initialization_failure_ };
        }

        foundation_truehdr_status_e status;
        {
          boost::lock_guard lock { adapter_mutex };
          status = loader_.api()->process(
            instance_,
            device_context_,
            input.texture,
            output_texture_.get());
        }
        if (status != FOUNDATION_TRUEHDR_STATUS_OK) {
          return {
            .status = filter_status_e::failed,
            .frame = {},
            .reason = truehdr_failure_reason(status, false),
          };
        }
        return make_scrgb_result(input, output_texture_.get(), output_srv_.get());
      }

      void
      flush() override {
        if (instance_) {
          boost::lock_guard lock { adapter_mutex };
          loader_.api()->flush(instance_);
        }
      }

      std::string_view
      backend_name() const override {
        return "alkaidlab.nvidia_rtx_video";
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

        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
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

        foundation_truehdr_config_t config {
          .struct_size = sizeof(foundation_truehdr_config_t),
          .width = width,
          .height = height,
          .contrast = config_.contrast,
          .saturation = config_.saturation,
          .middle_gray_nits = config_.middle_gray_nits,
          .peak_nits = config_.peak_nits,
          .runtime_directory = loader_.runtime_directory(),
        };
        foundation_truehdr_status_e status;
        {
          boost::lock_guard lock { adapter_mutex };
          status = loader_.api()->create(device_, &config, &instance_);
        }
        if (status != FOUNDATION_TRUEHDR_STATUS_OK || !instance_) {
          initialization_failure_ = status == FOUNDATION_TRUEHDR_STATUS_OK ? "backend_create_missing_instance" : truehdr_failure_reason(status, true);
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
      adapter_loader_t loader_;
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
    std::string &error) {
    adapter_loader_t loader;
    const auto adapter_path = path;
    const auto runtime_path = path.parent_path() / RUNTIME_FILENAME;
    if (!loader.load(adapter_path, runtime_path)) {
      error = loader.error();
      return {};
    }
    return std::make_unique<external_sdr_to_hdr_filter_t>(device, context, std::move(loader), config);
  }
}  // namespace platf::dxgi::hdr_enhanced::nvidia_rtx_video::truehdr
