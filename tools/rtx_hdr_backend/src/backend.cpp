// This software contains source code provided by NVIDIA Corporation.
// The NGX lifecycle and TrueHDR parameter contract follow the RTX Video SDK
// 1.1 DX11 sample distributed under the NVIDIA RTX SDKs License.

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <new>
#include <string>

#include <d3d10_1.h>
#include <d3d11_4.h>
#include <windows.h>

#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_defs_truehdr.h>
#include <nvsdk_ngx_helpers_truehdr.h>

#include "src/platform/windows/rtx_hdr/backend_abi.h"

namespace {
  std::mutex ngx_mutex;

  template <class T>
  void release(T *&value) {
    if (value) {
      value->Release();
      value = nullptr;
    }
  }

  std::wstring ngx_data_path() {
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
    explicit multithread_scope_t(ID3D10Multithread *multithread): multithread_ { multithread } {
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

    ~instance_t() {
      shutdown();
    }

    foundation_truehdr_status_e initialize(
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
      auto status = NVSDK_NGX_D3D11_Init(0, data_path.c_str(), device);
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
      return NVSDK_NGX_FAILED(status) || !feature
               ? FOUNDATION_TRUEHDR_STATUS_RUNTIME_UNAVAILABLE
               : FOUNDATION_TRUEHDR_STATUS_OK;
    }

    foundation_truehdr_status_e process(
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
      return NVSDK_NGX_FAILED(status)
               ? FOUNDATION_TRUEHDR_STATUS_INTERNAL_ERROR
               : FOUNDATION_TRUEHDR_STATUS_OK;
    }

    void flush() {
      if (context) {
        context->Flush();
      }
    }

    void shutdown() {
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

  foundation_truehdr_status_e FOUNDATION_TRUEHDR_CALL create_backend(
    void *device,
    const foundation_truehdr_config_t *config,
    void **instance) {
    if (!device || !config || !instance ||
        config->struct_size < sizeof(foundation_truehdr_config_t) ||
        config->width == 0 || config->height == 0) {
      return FOUNDATION_TRUEHDR_STATUS_INVALID_ARGUMENT;
    }
    *instance = nullptr;
    auto candidate = new (std::nothrow) instance_t;
    if (!candidate) {
      return FOUNDATION_TRUEHDR_STATUS_INTERNAL_ERROR;
    }
    const auto status = candidate->initialize(
      static_cast<ID3D11Device *>(device),
      *config);
    if (status != FOUNDATION_TRUEHDR_STATUS_OK) {
      delete candidate;
      return status;
    }
    *instance = candidate;
    return FOUNDATION_TRUEHDR_STATUS_OK;
  }

  foundation_truehdr_status_e FOUNDATION_TRUEHDR_CALL process_backend(
    void *instance,
    void *context,
    void *input,
    void *output) {
    if (!instance) {
      return FOUNDATION_TRUEHDR_STATUS_INVALID_ARGUMENT;
    }
    return static_cast<instance_t *>(instance)->process(
      static_cast<ID3D11DeviceContext *>(context),
      static_cast<ID3D11Texture2D *>(input),
      static_cast<ID3D11Texture2D *>(output));
  }

  void FOUNDATION_TRUEHDR_CALL flush_backend(void *instance) {
    if (instance) {
      static_cast<instance_t *>(instance)->flush();
    }
  }

  void FOUNDATION_TRUEHDR_CALL destroy_backend(void *instance) {
    delete static_cast<instance_t *>(instance);
  }
}

extern "C" __declspec(dllexport) const foundation_truehdr_api_t *FOUNDATION_TRUEHDR_CALL
foundation_truehdr_get_api(std::uint32_t requested_abi_version) {
  static const foundation_truehdr_api_t api {
    FOUNDATION_TRUEHDR_ABI_VERSION,
    sizeof(foundation_truehdr_api_t),
    create_backend,
    process_backend,
    flush_backend,
    destroy_backend,
  };
  return requested_abi_version == FOUNDATION_TRUEHDR_ABI_VERSION ? &api : nullptr;
}
