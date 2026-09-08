// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Foundation Sunshine contributors
//
// Project-authored adapter for the NVIDIA RTX Video SDK 1.1 TrueHDR API.
// NVIDIA headers, import libraries, and runtime binaries are external SDK
// components and are not part of this source file.

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <memory>
#include <new>
#include <string>

#include <d3d10_1.h>
#include <d3d11_4.h>
#include <windows.h>

#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_defs_truehdr.h>
#include <nvsdk_ngx_helpers_truehdr.h>

#include "src/platform/windows/hdr_enhanced/nvidia_rtx_video/bridge_abi.h"

namespace {
  std::mutex ngx_mutex;
  constexpr unsigned long long ngx_application_id = FOUNDATION_NGX_APPLICATION_ID;
  constexpr std::time_t bridge_build_time = FOUNDATION_RTX_VIDEO_BUILD_UNIX_SECONDS;
  constexpr std::time_t development_lifetime = 14 * 24 * 60 * 60;

  bool
  development_build_expired() noexcept {
    if constexpr (ngx_application_id != 0) return false;
    const auto now = std::time(nullptr);
    return now != static_cast<std::time_t>(-1) && now >= bridge_build_time &&
           now - bridge_build_time > development_lifetime;
  }

  template <class T>
  void
  release(T *&value) {
    if (value) {
      value->Release();
      value = nullptr;
    }
  }

  std::wstring
  ngx_data_path() {
    wchar_t buffer[MAX_PATH] {};
    const DWORD length = GetTempPathW(MAX_PATH, buffer);
    if (length == 0 || length >= MAX_PATH) {
      return L".";
    }
    std::filesystem::path path { std::wstring(buffer, length) };
    path /= L"foundation-sunshine-ngx";
    std::error_code error;
    std::filesystem::create_directories(path, error);
    return error ? L"." : path.wstring();
  }

  class multithread_scope_t {
  public:
    explicit multithread_scope_t(ID3D10Multithread *multithread):
        multithread_ { multithread } {
      if (multithread_) {
        multithread_->Enter();
      }
    }
    ~multithread_scope_t() {
      if (multithread_) {
        multithread_->Leave();
      }
    }

  private:
    ID3D10Multithread *multithread_;
  };

  struct instance_t {
    ID3D11Device *device = nullptr;
    ID3D11DeviceContext *context = nullptr;
    ID3D10Multithread *multithread = nullptr;
    NVSDK_NGX_Parameter *parameters = nullptr;
    NVSDK_NGX_Handle *feature = nullptr;
    foundation_truehdr_config_t config {};
    bool ngx_initialized = false;

    ~instance_t() noexcept {
      // SDK 清理异常不能穿过 DLL ABI，COM 引用仍由本模块释放。
      try {
        shutdown();
      }
      catch (...) {
        release(multithread);
        release(context);
        release(device);
      }
    }

    foundation_truehdr_status_e
    initialize(
      ID3D11Device *input_device,
      const foundation_truehdr_config_t &input_config) {
      device = input_device;
      device->AddRef();
      device->GetImmediateContext(&context);
      if (!context) {
        return FOUNDATION_TRUEHDR_STATUS_INVALID_ARGUMENT;
      }
      if (SUCCEEDED(context->QueryInterface(__uuidof(ID3D10Multithread), reinterpret_cast<void **>(&multithread)))) {
        multithread->SetMultithreadProtected(TRUE);
      }
      config = input_config;

      std::lock_guard lock { ngx_mutex };
      multithread_scope_t multithread_scope { multithread };
      const auto data_path = ngx_data_path();
      auto status = NVSDK_NGX_D3D11_Init(ngx_application_id, data_path.c_str(), device);
      if (NVSDK_NGX_FAILED(status)) {
        return FOUNDATION_TRUEHDR_STATUS_RUNTIME_UNAVAILABLE;
      }
      ngx_initialized = true;

      status = NVSDK_NGX_D3D11_GetCapabilityParameters(&parameters);
      if (NVSDK_NGX_FAILED(status) || !parameters) {
        return FOUNDATION_TRUEHDR_STATUS_RUNTIME_UNAVAILABLE;
      }
      int available = 0;
      status = parameters->Get(NVSDK_NGX_Parameter_TrueHDR_Available, &available);
      if (NVSDK_NGX_FAILED(status) || !available) {
        return FOUNDATION_TRUEHDR_STATUS_UNSUPPORTED;
      }
      size_t scratch_size = 0;
      status = NVSDK_NGX_D3D11_GetScratchBufferSize(
        NVSDK_NGX_Feature_TrueHDR,
        parameters,
        &scratch_size);
      if (NVSDK_NGX_FAILED(status) || scratch_size != 0) {
        return FOUNDATION_TRUEHDR_STATUS_UNSUPPORTED;
      }
      NVSDK_NGX_Feature_Create_Params create_params {};
      status = NGX_D3D11_CREATE_TRUEHDR_EXT(context, &feature, parameters, &create_params);
      return NVSDK_NGX_FAILED(status) || !feature ? FOUNDATION_TRUEHDR_STATUS_RUNTIME_UNAVAILABLE : FOUNDATION_TRUEHDR_STATUS_OK;
    }

    foundation_truehdr_status_e
    process(
      ID3D11DeviceContext *input_context,
      ID3D11Texture2D *input,
      ID3D11Texture2D *output) {
      if (!feature || !parameters || !input_context || !input || !output) {
        return FOUNDATION_TRUEHDR_STATUS_INVALID_ARGUMENT;
      }

      ID3D11Device *context_device = nullptr;
      input_context->GetDevice(&context_device);
      const bool same_device = context_device == device;
      release(context_device);
      if (!same_device) {
        return FOUNDATION_TRUEHDR_STATUS_INVALID_ARGUMENT;
      }
      const auto context_type = input_context->GetType();
      if (context_type != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
        return FOUNDATION_TRUEHDR_STATUS_INVALID_ARGUMENT;
      }

      D3D11_TEXTURE2D_DESC input_desc {};
      D3D11_TEXTURE2D_DESC output_desc {};
      input->GetDesc(&input_desc);
      output->GetDesc(&output_desc);
      const bool valid_input =
        input_desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
        input_desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
        // Same sampling semantics as BGRA8 with a padding alpha channel.
        input_desc.Format == DXGI_FORMAT_B8G8R8X8_UNORM;
      if (!valid_input || output_desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT ||
          !(output_desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) ||
          input_desc.Width != output_desc.Width || input_desc.Height != output_desc.Height ||
          input_desc.Width != config.width || input_desc.Height != config.height) {
        return FOUNDATION_TRUEHDR_STATUS_INVALID_ARGUMENT;
      }

      NVSDK_NGX_D3D11_TRUEHDR_Eval_Params params {};
      params.pInput = input;
      params.pOutput = output;
      params.InputSubrectTL = { 0, 0 };
      params.InputSubrectBR = { input_desc.Width, input_desc.Height };
      params.OutputSubrectTL = { 0, 0 };
      params.OutputSubrectBR = { output_desc.Width, output_desc.Height };
      params.Contrast = static_cast<unsigned int>(
        std::clamp(config.contrast + 100.0f, 0.0f, 200.0f));
      params.Saturation = static_cast<unsigned int>(
        std::clamp(config.saturation + 100.0f, 0.0f, 200.0f));
      params.MiddleGray = static_cast<unsigned int>(
        std::clamp(config.middle_gray_nits, 10.0f, 100.0f));
      params.MaxLuminance = static_cast<unsigned int>(
        std::clamp(config.peak_nits, 400.0f, 2000.0f));

      std::lock_guard lock { ngx_mutex };
      multithread_scope_t multithread_scope { multithread };
      const auto status = NGX_D3D11_EVALUATE_TRUEHDR_EXT(
        context,
        feature,
        parameters,
        &params);
      return NVSDK_NGX_FAILED(status) ? FOUNDATION_TRUEHDR_STATUS_INTERNAL_ERROR : FOUNDATION_TRUEHDR_STATUS_OK;
    }

    void
    flush() {
      if (context) {
        context->Flush();
      }
    }

    void
    shutdown() {
      std::lock_guard lock { ngx_mutex };
      {
        // Leave the D3D multithread section before releasing the interface
        // that owns it. Keeping the scope alive across release(multithread)
        // would make its destructor call Leave() on a released COM object.
        multithread_scope_t multithread_scope { multithread };
        if (feature) {
          NVSDK_NGX_D3D11_ReleaseFeature(feature);
          feature = nullptr;
        }
        // Preserve the teardown order used by the RTX Video SDK 1.1 DX11
        // TrueHDR samples: release feature, shut down NGX, then destroy the
        // capability parameters returned during initialization.
        if (ngx_initialized) {
          NVSDK_NGX_D3D11_Shutdown1(device);
          ngx_initialized = false;
        }
        if (parameters) {
          NVSDK_NGX_D3D11_DestroyParameters(parameters);
          parameters = nullptr;
        }
      }
      release(multithread);
      release(context);
      release(device);
    }
  };

  foundation_truehdr_status_e FOUNDATION_RTX_VIDEO_CALL
  create_truehdr(
    void *device,
    const foundation_truehdr_config_t *config,
    void **instance) noexcept {
    if (instance) *instance = nullptr;
    if (!device || !config || !instance ||
        config->struct_size < sizeof(foundation_truehdr_config_t) ||
        config->width == 0 || config->height == 0) {
      return FOUNDATION_TRUEHDR_STATUS_INVALID_ARGUMENT;
    }
    // Application ID 0 is permitted for development. Keep those bridge builds
    // time-limited so published evaluation artifacts do not become permanent releases.
    if (development_build_expired()) {
      return FOUNDATION_TRUEHDR_STATUS_DEVELOPMENT_BUILD_EXPIRED;
    }
    try {
      auto candidate = std::make_unique<instance_t>();
      const auto status = candidate->initialize(
        static_cast<ID3D11Device *>(device),
        *config);
      if (status != FOUNDATION_TRUEHDR_STATUS_OK) {
        return status;
      }
      *instance = candidate.release();
      return FOUNDATION_TRUEHDR_STATUS_OK;
    }
    catch (...) {
      return FOUNDATION_TRUEHDR_STATUS_INTERNAL_ERROR;
    }
  }

  foundation_truehdr_status_e FOUNDATION_RTX_VIDEO_CALL
  process_truehdr(
    void *instance,
    void *context,
    void *input,
    void *output) noexcept {
    if (!instance) {
      return FOUNDATION_TRUEHDR_STATUS_INVALID_ARGUMENT;
    }
    try {
      return static_cast<instance_t *>(instance)->process(
        static_cast<ID3D11DeviceContext *>(context),
        static_cast<ID3D11Texture2D *>(input),
        static_cast<ID3D11Texture2D *>(output));
    }
    catch (...) {
      return FOUNDATION_TRUEHDR_STATUS_INTERNAL_ERROR;
    }
  }

  void FOUNDATION_RTX_VIDEO_CALL
  flush_truehdr(void *instance) noexcept {
    try {
      if (instance) {
        static_cast<instance_t *>(instance)->flush();
      }
    }
    catch (...) {
    }
  }

  void FOUNDATION_RTX_VIDEO_CALL
  destroy_truehdr(void *instance) noexcept {
    delete static_cast<instance_t *>(instance);
  }
}  // namespace

extern "C" __declspec(dllexport) const foundation_truehdr_bridge_api_t *FOUNDATION_RTX_VIDEO_CALL
foundation_truehdr_bridge_get_api(std::uint32_t requested_abi_version) {
  static const foundation_truehdr_bridge_api_t api {
    FOUNDATION_TRUEHDR_BRIDGE_ABI_VERSION,
    sizeof(foundation_truehdr_bridge_api_t),
    create_truehdr,
    process_truehdr,
    flush_truehdr,
    destroy_truehdr,
  };
  return requested_abi_version == FOUNDATION_TRUEHDR_BRIDGE_ABI_VERSION ? &api : nullptr;
}
