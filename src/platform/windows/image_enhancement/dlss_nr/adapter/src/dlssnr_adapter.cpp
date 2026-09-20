/**
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 Foundation Sunshine contributors
 *
 * @file src/platform/windows/image_enhancement/dlss_nr/adapter/src/dlssnr_adapter.cpp
 * @brief DLSS NR (NGX feature 18) adapter: a private D3D12 device running the
 *        signed nvngx_dlssnr snippet against D3D11 shared resources.
 *
 * The integration contract mirrors the colour-only integration proven by the
 * Magpie-Experimental fork: same-resolution evaluation, an all-zero depth
 * texture, zero motion vectors unless the host enables optical flow, and an
 * IAT-level caller-compatibility shim so the signed snippet accepts this
 * adapter as its loader. Every NGX call is wrapped in SEH; a faulting runtime
 * latches into a permanent error state instead of taking the host down.
 */
#include "src/platform/windows/image_enhancement/dlss_nr/adapter_abi.h"
#include "nvof_provider.h"

#include <windows.h>

#include <d3d11.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi.h>
#include <dxgi1_2.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <type_traits>
#include <nvsdk_ngx.h>

#ifndef NVSDK_CONV
  #define NVSDK_CONV __cdecl
#endif

namespace {

  // ---------------------------------------------------------------------------
  // The public SDK supplies NGX types and the core parameter allocator.
  // Feature 18 entry points are resolved from the separately supplied snippet.
  // ---------------------------------------------------------------------------

  // Exports of the signed nvngx_dlssnr.dll snippet (resolved via GetProcAddress).
  typedef NVSDK_NGX_Result(NVSDK_CONV *SnippetInitExtFn)(
    unsigned long long InApplicationId,
    const wchar_t *InApplicationDataPath,
    ID3D12Device *InDevice,
    NVSDK_NGX_Version InSDKVersion,
    const NVSDK_NGX_Parameter *InParameters);
  typedef NVSDK_NGX_Result(NVSDK_CONV *AllocateParametersFn)(NVSDK_NGX_Parameter **OutParameters);
  typedef NVSDK_NGX_Result(NVSDK_CONV *DestroyParametersFn)(NVSDK_NGX_Parameter *InParameters);
  typedef NVSDK_NGX_Result(NVSDK_CONV *CreateFeatureFn)(
    ID3D12GraphicsCommandList *InCmdList,
    int InFeatureID,
    NVSDK_NGX_Parameter *InParameters,
    NVSDK_NGX_Handle **OutHandle);
  typedef NVSDK_NGX_Result(NVSDK_CONV *EvaluateFeatureFn)(
    ID3D12GraphicsCommandList *InCmdList,
    const NVSDK_NGX_Handle *InFeatureHandle,
    const NVSDK_NGX_Parameter *InParameters,
    PFN_NVSDK_NGX_ProgressCallback InCallback);
  typedef NVSDK_NGX_Result(NVSDK_CONV *ReleaseFeatureFn)(NVSDK_NGX_Handle *InHandle);

  constexpr NVSDK_NGX_Version DLSSNR_SDK_VERSION = NVSDK_NGX_Version_API;
  constexpr int DLSSNR_FEATURE_ID = 18;  // DLSS neural rendering (same resolution)
  constexpr unsigned long long DLSSNR_SNIPPET_APPLICATION_ID = 0x0876232Cull;
  constexpr NVSDK_NGX_Result NGX_PLATFORM_ERROR = NVSDK_NGX_Result_FAIL_PlatformError;

  inline bool
  ngx_succeeded(NVSDK_NGX_Result value) {
    return NVSDK_NGX_SUCCEED(value);
  }

  // DLSSNR.* parameter names: same-resolution filter contract.
  constexpr char PARAM_WIDTH[] = "DLSSNR.Width";
  constexpr char PARAM_HEIGHT[] = "DLSSNR.Height";
  constexpr char PARAM_INPUT_WIDTH[] = "DLSSNR.InputWidth";
  constexpr char PARAM_INPUT_HEIGHT[] = "DLSSNR.InputHeight";
  constexpr char PARAM_OUTPUT_WIDTH[] = "DLSSNR.OutputWidth";
  constexpr char PARAM_OUTPUT_HEIGHT[] = "DLSSNR.OutputHeight";
  constexpr char PARAM_OUTPUT_DOT_WIDTH[] = "DLSSNR.OutputDotWidth";
  constexpr char PARAM_OUTPUT_DOT_HEIGHT[] = "DLSSNR.OutputDotHeight";
  constexpr char PARAM_UPSCALING[] = "DLSSNR.Upscaling";
  constexpr char PARAM_SCALE[] = "DLSSNR.Scale";
  constexpr char PARAM_SCALING_RATIO[] = "DLSSNR.ScalingRatio";
  constexpr char PARAM_SCALING_RATIO_CALLBACK[] = "DLSSNRComputeScalingRatioCallback";
  constexpr char PARAM_PRESET[] = "DLSSNR.Hint.Render.Preset";
  constexpr char PARAM_COLOR[] = "DLSSNR.Color";
  constexpr char PARAM_OUTPUT[] = "DLSSNR.Output";
  constexpr char PARAM_MVEC[] = "DLSSNR.MVec";
  constexpr char PARAM_DEPTH[] = "DLSSNR.Depth";
  constexpr char PARAM_MVEC_SCALE_X[] = "DLSSNR.MVecScaleX";
  constexpr char PARAM_MVEC_SCALE_Y[] = "DLSSNR.MVecScaleY";
  constexpr char PARAM_DEPTH_INVERTED[] = "DLSSNR.DepthInverted";
  constexpr char PARAM_ENABLED[] = "DLSSNR.Enabled";
  constexpr char PARAM_RESET[] = "DLSSNR.Reset";
  constexpr char PARAM_STYLE[] = "DLSSNR.Style";
  constexpr char PARAM_INTENSITY[] = "DLSSNR.Intensity";
  constexpr char PARAM_LOCAL_TONE[] = "DLSSNR.LocalToneStrength";
  constexpr char PARAM_LOCAL_STRUCTURE[] = "DLSSNR.LocalStructureStrength";
  constexpr char PARAM_SKIN_STRUCTURE[] = "DLSSNR.SkinStructureStrength";
  constexpr char PARAM_AUTO_MASK[] = "DLSSNR.UseAutoMask";
  constexpr char PARAM_UI_CORRECTION[] = "DLSSNR.UICorrection";
  constexpr char PARAM_INDICATOR_INVERT_X[] = "DLSS.Indicator.Invert.X.Axis";
  constexpr char PARAM_INDICATOR_INVERT_Y[] = "DLSS.Indicator.Invert.Y.Axis";
  constexpr char PARAM_PERF_QUALITY[] = "PerfQualityValue";
  constexpr char PARAM_CREATION_NODE_MASK[] = "CreationNodeMask";
  constexpr char PARAM_VISIBILITY_NODE_MASK[] = "VisibilityNodeMask";
  constexpr char PARAM_WIDTH_COMMON[] = "Width";
  constexpr char PARAM_HEIGHT_COMMON[] = "Height";

  // ---------------------------------------------------------------------------
  // SEH guards. The snippet runs driver code that may fault on unsupported
  // configurations; the latch keeps a bad runtime from killing the host.
  // ---------------------------------------------------------------------------

  std::atomic<bool> g_faulted { false };

  // MSVC 19.39 /O2 fails the injected-exception regression at this SEH
  // boundary. Keep the boundary out of line and unoptimized; the caller and
  // all GPU work retain normal optimization.
  #pragma optimize("", off)
  __declspec(noinline) NVSDK_NGX_Result
  ngx_invoke_raw(NVSDK_NGX_Result (*body)(void *), void *context) noexcept {
    if (g_faulted.load(std::memory_order_acquire)) {
      return NGX_PLATFORM_ERROR;
    }
    NVSDK_NGX_Result result = NGX_PLATFORM_ERROR;
    __try {
      result = body(context);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
      g_faulted.store(true, std::memory_order_release);
      return NGX_PLATFORM_ERROR;
    }
    return result;
  }
  #pragma optimize("", on)

  template <typename Fn>
  NVSDK_NGX_Result
  ngx_invoke(Fn &&body) noexcept {
    return ngx_invoke_raw([](void *context) -> NVSDK_NGX_Result {
      return (*static_cast<std::remove_reference_t<Fn> *>(context))();
    }, &body);
  }

  NVSDK_NGX_Result
  ngx_set_scaling_ratio_callback(NVSDK_NGX_Parameter *parameters) noexcept {
    return ngx_invoke([&] {
      if (!parameters) return NGX_PLATFORM_ERROR;
      parameters->Set(PARAM_SCALING_RATIO, 1.0f);
      return NVSDK_NGX_Result_Success;
    });
  }

  // ---------------------------------------------------------------------------
  // Caller compatibility. The signed snippet verifies it was loaded by the
  // official NGX loader (nvngx.dll); report that name when the snippet
  // queries this adapter through GetModuleFileNameW.
  // ---------------------------------------------------------------------------

  std::atomic<HMODULE> g_hook_module { nullptr };
  std::atomic<void **> g_hook_slot { nullptr };
  std::atomic<void *> g_hook_original { nullptr };
  SRWLOCK g_hook_lock = SRWLOCK_INIT;

  void **
  find_imported_function_slot(HMODULE module, const char *function_name) noexcept {
    if (!module || !function_name) return nullptr;
    auto *base = reinterpret_cast<std::byte *>(module);
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return nullptr;
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
      return nullptr;
    }

    const auto &directory =
      nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress || !directory.Size ||
        directory.VirtualAddress >= nt->OptionalHeader.SizeOfImage ||
        directory.Size > nt->OptionalHeader.SizeOfImage ||
        directory.VirtualAddress > nt->OptionalHeader.SizeOfImage - directory.Size) {
      return nullptr;
    }

    auto *descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(
      base + directory.VirtualAddress);
    const auto *descriptor_end = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR *>(
      base + directory.VirtualAddress + directory.Size);
    for (; descriptor < descriptor_end && descriptor->Name; ++descriptor) {
      if (descriptor->Name >= nt->OptionalHeader.SizeOfImage) continue;
      const char *library = reinterpret_cast<const char *>(base + descriptor->Name);
      if (_stricmp(library, "KERNEL32.dll") != 0 &&
          _stricmp(library, "api-ms-win-core-libraryloader-l1-2-0.dll") != 0 &&
          _stricmp(library, "api-ms-win-core-libraryloader-l1-1-0.dll") != 0) {
        continue;
      }
      if (!descriptor->OriginalFirstThunk || !descriptor->FirstThunk) continue;
      auto *name_thunk = reinterpret_cast<IMAGE_THUNK_DATA64 *>(
        base + descriptor->OriginalFirstThunk);
      auto *address_thunk = reinterpret_cast<IMAGE_THUNK_DATA64 *>(
        base + descriptor->FirstThunk);
      for (; name_thunk->u1.AddressOfData; ++name_thunk, ++address_thunk) {
        if (IMAGE_SNAP_BY_ORDINAL64(name_thunk->u1.Ordinal)) continue;
        const uint32_t name_rva = static_cast<uint32_t>(name_thunk->u1.AddressOfData);
        if (name_rva >= nt->OptionalHeader.SizeOfImage) return nullptr;
        const auto *import = reinterpret_cast<const IMAGE_IMPORT_BY_NAME *>(base + name_rva);
        if (std::strcmp(reinterpret_cast<const char *>(import->Name), function_name) == 0) {
          return reinterpret_cast<void **>(&address_thunk->u1.Function);
        }
      }
    }
    return nullptr;
  }

  DWORD WINAPI
  hook_get_module_file_name_w(HMODULE module, LPWSTR filename, DWORD size) noexcept {
    constexpr wchar_t AUTHORIZED_CALLER[] = L"nvngx.dll";
    if (module != nullptr && module == g_hook_module.load(std::memory_order_acquire)) {
      const DWORD authorized_length = static_cast<DWORD>(sizeof(AUTHORIZED_CALLER) / sizeof(wchar_t) - 1);
      if (!filename || !size) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0;
      }
      if (size <= authorized_length) {
        if (size > 1) {
          std::memcpy(filename, AUTHORIZED_CALLER, (size - 1) * sizeof(wchar_t));
        }
        filename[size - 1] = L'\0';
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return size;
      }
      std::memcpy(filename, AUTHORIZED_CALLER, sizeof(AUTHORIZED_CALLER));
      return authorized_length;
    }
    const auto original = reinterpret_cast<decltype(&hook_get_module_file_name_w)>(
      g_hook_original.load(std::memory_order_acquire));
    if (original) return original(module, filename, size);
    SetLastError(ERROR_INVALID_FUNCTION);
    return 0;
  }

  bool
  install_caller_compatibility(HMODULE snippet_module) noexcept {
    struct lock_guard_t {
      lock_guard_t() { AcquireSRWLockExclusive(&g_hook_lock); }
      ~lock_guard_t() { ReleaseSRWLockExclusive(&g_hook_lock); }
    } lock;

    void **slot = find_imported_function_slot(snippet_module, "GetModuleFileNameW");
    if (!slot) return false;

    void *hook = reinterpret_cast<void *>(&hook_get_module_file_name_w);
    // The snippet stays loaded across sessions. Replacing an already patched
    // slot would save our own hook as its original and recurse on other modules.
    if (*slot == hook) {
      return g_hook_original.load(std::memory_order_acquire) != nullptr;
    }
    // One process-wide original cannot represent multiple runtime imports.
    if (g_hook_slot.load(std::memory_order_acquire)) return false;

    HMODULE owner = nullptr;
    if (!GetModuleHandleExW(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
          reinterpret_cast<LPCWSTR>(hook), &owner)) {
      return false;
    }

    DWORD old_protection = 0;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old_protection)) {
      return false;
    }
    g_hook_module.store(owner, std::memory_order_release);
    // Publish the original before installing the hook: the snippet may call
    // the import as soon as it changes. Pin the owner above because the snippet
    // retains this function pointer even after the host unloads its adapter.
    void *original = *slot;
    if (!original) {
      DWORD ignored = 0;
      VirtualProtect(slot, sizeof(void *), old_protection, &ignored);
      return false;
    }
    g_hook_original.store(original, std::memory_order_release);
    InterlockedExchangePointer(reinterpret_cast<void *volatile *>(slot), hook);
    g_hook_slot.store(slot, std::memory_order_release);
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void *), old_protection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void *));
    return original != nullptr;
  }

  // ---------------------------------------------------------------------------
  // Instance state and helpers.
  // ---------------------------------------------------------------------------

  struct instance_t {
    ID3D11Device *device11 = nullptr;
    ID3D11DeviceContext4 *context11 = nullptr;

    ID3D12Device *device12 = nullptr;
    ID3D12CommandQueue *queue12 = nullptr;
    ID3D12CommandAllocator *allocator12 = nullptr;
    ID3D12GraphicsCommandList *list12 = nullptr;

    ID3D11Fence *fence11 = nullptr;
    ID3D12Fence *fence12 = nullptr;
    HANDLE fence_event = nullptr;
    uint64_t fence_value = 0;

    ID3D11Texture2D *input_mirror11 = nullptr;
    ID3D11Texture2D *output_mirror11 = nullptr;
    ID3D11Texture2D *motion11 = nullptr;
    ID3D11Texture2D *zero_depth11 = nullptr;
    ID3D12Resource *input_mirror12 = nullptr;
    ID3D12Resource *output_mirror12 = nullptr;
    ID3D12Resource *motion12 = nullptr;
    ID3D12Resource *zero_depth12 = nullptr;

    HMODULE snippet = nullptr;
    AllocateParametersFn allocate_parameters = nullptr;
    DestroyParametersFn destroy_parameters = nullptr;
    CreateFeatureFn create_feature = nullptr;
    EvaluateFeatureFn evaluate_feature = nullptr;
    ReleaseFeatureFn release_feature = nullptr;
    NVSDK_NGX_Parameter *parameters = nullptr;
    NVSDK_NGX_Handle *feature = nullptr;

    uint32_t width = 0;
    uint32_t height = 0;
    foundation_dlssnr_config_t config {};
    std::unique_ptr<flow_provider> optical_flow;
    bool first_frame = true;
    std::wstring runtime_directory;
    std::wstring data_path;

    // Optional diagnostics; collect after the existing submission fence, so
    // timing adds no CPU/GPU wait to the normal processing path.
    ID3D12QueryHeap *timing_heap = nullptr;
    ID3D12Resource *timing_readback = nullptr;
    uint64_t timing_frequency = 0;
    bool timing_pending = false;
    uint64_t timing_frames = 0;
    uint64_t timing_samples = 0;
    double timing_total_ms = 0;
    double timing_min_ms = 0;
    double timing_max_ms = 0;
  };

  template <typename T>
  void
  safe_release(T *&value) noexcept {
    if (value) {
      value->Release();
      value = nullptr;
    }
  }

  HRESULT
  open_shared_12(ID3D12Device *device12, ID3D11Texture2D *texture11, ID3D12Resource **out) noexcept {
    IDXGIResource1 *dxgi_resource = nullptr;
    HRESULT hr = texture11->QueryInterface(IID_PPV_ARGS(&dxgi_resource));
    if (FAILED(hr)) return hr;
    HANDLE handle = nullptr;
    hr = dxgi_resource->CreateSharedHandle(nullptr,
      DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle);
    dxgi_resource->Release();
    if (FAILED(hr)) return hr;
    hr = device12->OpenSharedHandle(handle, IID_PPV_ARGS(out));
    CloseHandle(handle);
    return hr;
  }

  HRESULT
  create_shared_texture_11(
    ID3D11Device *device,
    DXGI_FORMAT format,
    uint32_t width,
    uint32_t height,
    ID3D11Texture2D **out) noexcept {
    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    const HRESULT hr = device->CreateTexture2D(&desc, nullptr, out);
    if (FAILED(hr)) {
      std::fprintf(stderr, "DLSS NR: CreateTexture2D format=%u failed: 0x%08lX\n",
        static_cast<unsigned>(format), static_cast<unsigned long>(hr));
    }
    return hr;
  }

  HRESULT
  clear_zero_texture_11(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11Texture2D *texture) noexcept {
    ID3D11UnorderedAccessView *uav = nullptr;
    HRESULT hr = device->CreateUnorderedAccessView(texture, nullptr, &uav);
    if (FAILED(hr)) return hr;
    const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    context->ClearUnorderedAccessViewFloat(uav, zero);
    uav->Release();
    return S_OK;
  }

  bool
  wait_cpu_fence(instance_t *instance, uint64_t value) noexcept {
    if (!instance->fence12 || instance->fence12->GetCompletedValue() >= value) {
      return true;
    }
    if (!instance->fence_event) return false;
    ResetEvent(instance->fence_event);
    if (FAILED(instance->fence12->SetEventOnCompletion(value, instance->fence_event))) {
      return false;
    }
    return WaitForSingleObject(instance->fence_event, 5000) == WAIT_OBJECT_0;
  }

  // ---------------------------------------------------------------------------
  // Adapter lifecycle.
  // ---------------------------------------------------------------------------

  void
  initialize_timing(instance_t *instance) noexcept {
    wchar_t enabled[2] {};
    if (GetEnvironmentVariableW(L"SUNSHINE_DLSSNR_TIMING", enabled, 2) != 1 || enabled[0] != L'1') return;
    D3D12_QUERY_HEAP_DESC query {};
    query.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    query.Count = 2;
    HRESULT hr = instance->queue12->GetTimestampFrequency(&instance->timing_frequency);
    if (SUCCEEDED(hr)) hr = instance->device12->CreateQueryHeap(&query, IID_PPV_ARGS(&instance->timing_heap));
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer {};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = 2 * sizeof(uint64_t);
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (SUCCEEDED(hr)) hr = instance->device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
      &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&instance->timing_readback));
    if (FAILED(hr) || !instance->timing_frequency) {
      std::fprintf(stderr, "DLSS NR: GPU timing unavailable: 0x%08lX\n", static_cast<unsigned long>(hr));
      safe_release(instance->timing_heap);
      safe_release(instance->timing_readback);
    }
  }

  void
  collect_timing(instance_t *instance) noexcept {
    if (!instance->timing_pending) return;
    instance->timing_pending = false;
    const D3D12_RANGE read_range { 0, 2 * sizeof(uint64_t) };
    void *mapped = nullptr;
    if (FAILED(instance->timing_readback->Map(0, &read_range, &mapped))) return;
    uint64_t ticks[2];
    std::memcpy(ticks, mapped, sizeof(ticks));
    const D3D12_RANGE no_writes { 0, 0 };
    instance->timing_readback->Unmap(0, &no_writes);
    // Exclude initialization/warmup from the steady-state measurement.
    if (++instance->timing_frames <= 10 || ticks[1] < ticks[0]) return;
    const double ms = double(ticks[1] - ticks[0]) * 1000.0 / double(instance->timing_frequency);
    if (!instance->timing_samples || ms < instance->timing_min_ms) instance->timing_min_ms = ms;
    if (!instance->timing_samples || ms > instance->timing_max_ms) instance->timing_max_ms = ms;
    ++instance->timing_samples;
    instance->timing_total_ms += ms;
  }

  foundation_dlssnr_status_e
  adapter_create(ID3D11Device *device, const foundation_dlssnr_config_t *config, void **out_instance);

  foundation_dlssnr_status_e
  adapter_process(void *raw_instance, void *device_context, void *input_texture, void *output_texture) noexcept;

  void adapter_flush(void *raw_instance) noexcept;

  void
  adapter_destroy(void *raw_instance) noexcept {
    auto *instance = static_cast<instance_t *>(raw_instance);
    if (!instance) return;
    adapter_flush(instance);
    instance->optical_flow.reset();
    if (instance->timing_samples) {
      std::fprintf(stderr, "DLSS NR: GPU Evaluate %ux%u samples=%llu warmup=10 avg=%.3f min=%.3f max=%.3f ms\n",
        instance->width, instance->height, static_cast<unsigned long long>(instance->timing_samples),
        instance->timing_total_ms / double(instance->timing_samples), instance->timing_min_ms, instance->timing_max_ms);
    }
    // Release the feature through SEH, then tear down COM state. The snippet
    // itself stays loaded for the process lifetime: Shutdown1/FreeLibrary on a
    // runtime the driver may still reference is not survivable.
    ngx_invoke([&] {
      if (instance->feature && instance->release_feature) {
        instance->release_feature(instance->feature);
      }
      return NVSDK_NGX_Result_Success;
    });
    if (instance->parameters && instance->destroy_parameters) {
      ngx_invoke([&] {
        return instance->destroy_parameters(instance->parameters);
      });
    }
    // NGX objects are opaque driver allocations, not COM: parameters go
    // through DestroyParameters, handles through ReleaseFeature above.
    instance->feature = nullptr;
    instance->parameters = nullptr;
    safe_release(instance->input_mirror12);
    safe_release(instance->output_mirror12);
    safe_release(instance->motion12);
    safe_release(instance->zero_depth12);
    safe_release(instance->input_mirror11);
    safe_release(instance->output_mirror11);
    safe_release(instance->motion11);
    safe_release(instance->zero_depth11);
    safe_release(instance->list12);
    safe_release(instance->timing_heap);
    safe_release(instance->timing_readback);
    safe_release(instance->allocator12);
    safe_release(instance->queue12);
    safe_release(instance->fence11);
    safe_release(instance->fence12);
    if (instance->fence_event) {
      CloseHandle(instance->fence_event);
      instance->fence_event = nullptr;
    }
    safe_release(instance->context11);
    safe_release(instance->device12);
    if (instance->device11) {
      instance->device11->Release();
      instance->device11 = nullptr;
    }
    // The snippet module intentionally stays in the address space; see above.
    delete instance;
  }

  foundation_dlssnr_status_e
  adapter_create(ID3D11Device *device, const foundation_dlssnr_config_t *config, void **out_instance) {
    if (!device || !config || !out_instance) {
      return FOUNDATION_DLSSNR_STATUS_INVALID_ARGUMENT;
    }
    *out_instance = nullptr;
    if (config->struct_size < sizeof(foundation_dlssnr_config_t) ||
        config->width == 0 || config->height == 0 ||
        !config->runtime_directory) {
      return FOUNDATION_DLSSNR_STATUS_INVALID_ARGUMENT;
    }
    if (g_faulted.load(std::memory_order_acquire)) {
      return FOUNDATION_DLSSNR_STATUS_RUNTIME_UNAVAILABLE;
    }

    auto *instance = new (std::nothrow) instance_t();
    if (!instance) return FOUNDATION_DLSSNR_STATUS_INTERNAL_ERROR;
    std::unique_ptr<instance_t, decltype(&adapter_destroy)> owner(instance, adapter_destroy);
    instance->device11 = device;
    device->AddRef();
    instance->runtime_directory = config->runtime_directory;
    instance->width = config->width;
    instance->height = config->height;
    instance->config = *config;

    foundation_dlssnr_status_e status = FOUNDATION_DLSSNR_STATUS_OK;
    HRESULT hr = S_OK;
    do {
      ID3D11DeviceContext *immediate = nullptr;
      device->GetImmediateContext(&immediate);
      if (immediate) {
        hr = immediate->QueryInterface(IID_PPV_ARGS(&instance->context11));
        immediate->Release();
      }
      else {
        hr = E_NOINTERFACE;
      }
      if (FAILED(hr)) { status = FOUNDATION_DLSSNR_STATUS_INVALID_ARGUMENT; break; }

      // The D3D11 fence lives on Device5; immediate contexts with fences on 4.
      ID3D11Device5 *device5 = nullptr;
      hr = device->QueryInterface(IID_PPV_ARGS(&device5));
      if (SUCCEEDED(hr)) {
        hr = device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&instance->fence11));
        device5->Release();
      }
      if (FAILED(hr)) { status = FOUNDATION_DLSSNR_STATUS_INTERNAL_ERROR; break; }

      // Resolve the adapter the encode device runs on and put the private
      // D3D12 device on the same GPU.
      IDXGIDevice *dxgi_device = nullptr;
      IDXGIAdapter *adapter = nullptr;
      DXGI_ADAPTER_DESC adapter_desc {};
      hr = device->QueryInterface(IID_PPV_ARGS(&dxgi_device));
      if (SUCCEEDED(hr)) hr = dxgi_device->GetAdapter(&adapter);
      if (SUCCEEDED(hr)) hr = adapter->GetDesc(&adapter_desc);
      if (dxgi_device) dxgi_device->Release();
      IDXGIFactory4 *factory = nullptr;
      IDXGIAdapter1 *resolved = nullptr;
      if (SUCCEEDED(hr)) hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
      if (SUCCEEDED(hr)) hr = factory->EnumAdapterByLuid(adapter_desc.AdapterLuid, IID_PPV_ARGS(&resolved));
      if (factory) factory->Release();
      if (adapter) adapter->Release();
      if (FAILED(hr)) { status = FOUNDATION_DLSSNR_STATUS_DEVICE_LOST; break; }
      hr = D3D12CreateDevice(resolved, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&instance->device12));
      resolved->Release();
      if (FAILED(hr)) { status = FOUNDATION_DLSSNR_STATUS_DEVICE_LOST; break; }

      D3D12_COMMAND_QUEUE_DESC queue_desc {};
      hr = instance->device12->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&instance->queue12));
      if (SUCCEEDED(hr)) hr = instance->device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&instance->allocator12));
      if (SUCCEEDED(hr)) hr = instance->device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, instance->allocator12, nullptr, IID_PPV_ARGS(&instance->list12));
      if (SUCCEEDED(hr)) hr = instance->list12->Close();
      if (FAILED(hr)) { status = FOUNDATION_DLSSNR_STATUS_INTERNAL_ERROR; break; }

      // Shared fence: D3D11 owns it, D3D12 opens it; both sides wait GPU-side.
      {
        HANDLE fence_handle = nullptr;
        hr = instance->fence11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fence_handle);
        if (SUCCEEDED(hr)) {
          hr = instance->device12->OpenSharedHandle(fence_handle, IID_PPV_ARGS(&instance->fence12));
          CloseHandle(fence_handle);
        }
      }
      if (SUCCEEDED(hr)) {
        instance->fence_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!instance->fence_event) hr = E_FAIL;
      }
      if (FAILED(hr)) { status = FOUNDATION_DLSSNR_STATUS_INTERNAL_ERROR; break; }

      // Cross-device mirrors (NGX reads color, writes output) and the
      // zero-contract guidance textures.
      hr = create_shared_texture_11(instance->device11, DXGI_FORMAT_B8G8R8A8_UNORM,
        instance->width, instance->height, &instance->input_mirror11);
      if (SUCCEEDED(hr)) hr = open_shared_12(instance->device12, instance->input_mirror11, &instance->input_mirror12);
      if (SUCCEEDED(hr)) hr = create_shared_texture_11(instance->device11, DXGI_FORMAT_B8G8R8A8_UNORM,
        instance->width, instance->height, &instance->output_mirror11);
      if (SUCCEEDED(hr)) hr = open_shared_12(instance->device12, instance->output_mirror11, &instance->output_mirror12);
      if (SUCCEEDED(hr)) hr = create_shared_texture_11(instance->device11, DXGI_FORMAT_R16G16_FLOAT,
        instance->width, instance->height, &instance->motion11);
      if (SUCCEEDED(hr)) hr = open_shared_12(instance->device12, instance->motion11, &instance->motion12);
      if (SUCCEEDED(hr)) hr = create_shared_texture_11(instance->device11, DXGI_FORMAT_R32_FLOAT,
        instance->width, instance->height, &instance->zero_depth11);
      if (SUCCEEDED(hr)) hr = open_shared_12(instance->device12, instance->zero_depth11, &instance->zero_depth12);
      if (SUCCEEDED(hr)) hr = clear_zero_texture_11(instance->device11, instance->context11, instance->motion11);
      if (SUCCEEDED(hr)) hr = clear_zero_texture_11(instance->device11, instance->context11, instance->zero_depth11);
      if (FAILED(hr)) { status = FOUNDATION_DLSSNR_STATUS_INTERNAL_ERROR; break; }

      // Load the explicitly selected snippet. Resolve imports only from System32,
      // never from an unverified DLL beside the runtime or the host executable.
      if (config->motion_mode == FOUNDATION_DLSSNR_MOTION_OPTICAL_FLOW) {
        instance->optical_flow = flow_provider::create(instance->device11, instance->context11,
          instance->input_mirror11, instance->motion11, config->motion_quality);
        if (!instance->optical_flow) {
          std::fprintf(stderr, "DLSS NR: requested NVOF provider unavailable\n");
          status = FOUNDATION_DLSSNR_STATUS_RUNTIME_UNAVAILABLE;
          break;
        }
        std::fprintf(stderr, "DLSS NR: half-resolution NVOF enabled, quality=%d\n", config->motion_quality);
      }
      const std::wstring dll_path = instance->runtime_directory + L"\\nvngx_dlssnr.dll";
      instance->snippet = LoadLibraryExW(dll_path.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_SYSTEM32);
      if (!instance->snippet) { status = FOUNDATION_DLSSNR_STATUS_RUNTIME_UNAVAILABLE; break; }
      if (!install_caller_compatibility(instance->snippet)) {
        status = FOUNDATION_DLSSNR_STATUS_RUNTIME_UNAVAILABLE;
        break;
      }

      const auto init_ext = reinterpret_cast<SnippetInitExtFn>(
        GetProcAddress(instance->snippet, "NVSDK_NGX_D3D12_Init_Ext"));
      instance->allocate_parameters = &NVSDK_NGX_D3D12_AllocateParameters;
      instance->create_feature = reinterpret_cast<CreateFeatureFn>(
        GetProcAddress(instance->snippet, "NVSDK_NGX_D3D12_CreateFeature"));
      instance->evaluate_feature = reinterpret_cast<EvaluateFeatureFn>(
        GetProcAddress(instance->snippet, "NVSDK_NGX_D3D12_EvaluateFeature"));
      instance->release_feature = reinterpret_cast<ReleaseFeatureFn>(
        GetProcAddress(instance->snippet, "NVSDK_NGX_D3D12_ReleaseFeature"));
      instance->destroy_parameters = &NVSDK_NGX_D3D12_DestroyParameters;
      if (!init_ext || !instance->allocate_parameters || !instance->create_feature ||
          !instance->evaluate_feature) {
        status = FOUNDATION_DLSSNR_STATUS_RUNTIME_UNAVAILABLE;
        break;
      }

      // Writable scratch directory for NGX logs, next to the runtime.
      instance->data_path = instance->runtime_directory + L"\\foundation-dlssnr-ngx";
      CreateDirectoryW(instance->data_path.c_str(), nullptr);

      const NVSDK_NGX_Result core_result = ngx_invoke([&] {
        return NVSDK_NGX_D3D12_Init_with_ProjectID(
          "ae3a6985-0b25-4ca9-b3f7-70ce4fa598a7", NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0",
          instance->data_path.c_str(), instance->device12);
      });
      std::fprintf(stderr, "DLSS NR: core Init result=0x%08X\n", static_cast<unsigned>(core_result));
      if (!ngx_succeeded(core_result)) { status = FOUNDATION_DLSSNR_STATUS_RUNTIME_UNAVAILABLE; break; }

      const NVSDK_NGX_Result init_result = ngx_invoke([&] {
        return init_ext(DLSSNR_SNIPPET_APPLICATION_ID, instance->data_path.c_str(),
          instance->device12, DLSSNR_SDK_VERSION, nullptr);
      });
      std::fprintf(stderr, "DLSS NR: snippet Init_Ext result=0x%08X\n", static_cast<unsigned>(init_result));
      if (!ngx_succeeded(init_result)) { status = FOUNDATION_DLSSNR_STATUS_RUNTIME_UNAVAILABLE; break; }

      const NVSDK_NGX_Result allocate_result = ngx_invoke([&] {
        return instance->allocate_parameters(&instance->parameters);
      });
      std::fprintf(stderr, "DLSS NR: AllocateParameters result=0x%08X\n", static_cast<unsigned>(allocate_result));
      if (!ngx_succeeded(allocate_result) || !instance->parameters) {
        status = FOUNDATION_DLSSNR_STATUS_RUNTIME_UNAVAILABLE;
        break;
      }

      // Same-resolution creation contract.
      const uint32_t w = instance->width;
      const uint32_t h = instance->height;
      const NVSDK_NGX_Result param_result = ngx_invoke([&] {
        instance->parameters->Set(PARAM_WIDTH, w);
        instance->parameters->Set(PARAM_HEIGHT, h);
        instance->parameters->Set(PARAM_INPUT_WIDTH, w);
        instance->parameters->Set(PARAM_INPUT_HEIGHT, h);
        instance->parameters->Set(PARAM_OUTPUT_WIDTH, w);
        instance->parameters->Set(PARAM_OUTPUT_HEIGHT, h);
        instance->parameters->Set(PARAM_OUTPUT_DOT_WIDTH, w);
        instance->parameters->Set(PARAM_OUTPUT_DOT_HEIGHT, h);
        instance->parameters->Set(PARAM_UPSCALING, 0u);
        instance->parameters->Set(PARAM_SCALE, 1.0f);
        instance->parameters->Set(PARAM_SCALING_RATIO, 1.0f);
        instance->parameters->Set(
          PARAM_SCALING_RATIO_CALLBACK,
          reinterpret_cast<void *>(&ngx_set_scaling_ratio_callback));
        instance->parameters->Set(PARAM_PRESET, 0u);
        instance->parameters->Set(PARAM_WIDTH_COMMON, w);
        instance->parameters->Set(PARAM_HEIGHT_COMMON, h);
        instance->parameters->Set(PARAM_PERF_QUALITY, 1u);  // Balanced
        instance->parameters->Set(PARAM_CREATION_NODE_MASK, 1u);
        instance->parameters->Set(PARAM_VISIBILITY_NODE_MASK, 1u);
        return NVSDK_NGX_Result_Success;
      });
      if (!ngx_succeeded(param_result)) { status = FOUNDATION_DLSSNR_STATUS_INTERNAL_ERROR; break; }

      // CreateFeature records into a reset command list; the submission is
      // drained before the first evaluate.
      hr = instance->list12->Reset(instance->allocator12, nullptr);
      if (FAILED(hr)) { status = FOUNDATION_DLSSNR_STATUS_INTERNAL_ERROR; break; }
      const NVSDK_NGX_Result create_result = ngx_invoke([&] {
        return instance->create_feature(instance->list12, DLSSNR_FEATURE_ID, instance->parameters, &instance->feature);
      });
      std::fprintf(stderr, "DLSS NR: CreateFeature result=0x%08X\n", static_cast<unsigned>(create_result));
      hr = instance->list12->Close();
      if (FAILED(hr)) { status = FOUNDATION_DLSSNR_STATUS_INTERNAL_ERROR; break; }
      if (!ngx_succeeded(create_result) || !instance->feature) {
        status = FOUNDATION_DLSSNR_STATUS_UNSUPPORTED;
        break;
      }
      const uint64_t ready_value = ++instance->fence_value;
      instance->queue12->ExecuteCommandLists(1, reinterpret_cast<ID3D12CommandList *const *>(&instance->list12));
      hr = instance->queue12->Signal(instance->fence12, ready_value);
      if (FAILED(hr)) { status = FOUNDATION_DLSSNR_STATUS_DEVICE_LOST; break; }
      if (!wait_cpu_fence(instance, ready_value)) {
        status = FOUNDATION_DLSSNR_STATUS_INTERNAL_ERROR;
        break;
      }

      initialize_timing(instance);
      *out_instance = owner.release();
      return FOUNDATION_DLSSNR_STATUS_OK;
    } while (false);

    // Route through destroy for a consistent teardown of everything this call
    // created; it releases any partially created NGX state.
    std::fprintf(stderr, "DLSS NR: create failed, status=%d HRESULT=0x%08lX\n",
      static_cast<int>(status), static_cast<unsigned long>(hr));
    return status;
  }

  foundation_dlssnr_status_e
  adapter_process(void *raw_instance, void *device_context, void *input_texture, void *output_texture) noexcept {
    auto *instance = static_cast<instance_t *>(raw_instance);
    if (!instance || !device_context || !input_texture || !output_texture) {
      return FOUNDATION_DLSSNR_STATUS_INVALID_ARGUMENT;
    }
    if (!instance->feature || !instance->parameters || !instance->evaluate_feature) {
      return FOUNDATION_DLSSNR_STATUS_RUNTIME_UNAVAILABLE;
    }
    auto *context = static_cast<ID3D11DeviceContext *>(device_context);
    auto *input = static_cast<ID3D11Texture2D *>(input_texture);
    auto *output = static_cast<ID3D11Texture2D *>(output_texture);

    // Same immediate context and device as create; geometry must match.
    ID3D11Device *context_device = nullptr;
    context->GetDevice(&context_device);
    const bool same_device = context_device == instance->device11;
    if (context_device) context_device->Release();
    if (!same_device || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
      return FOUNDATION_DLSSNR_STATUS_INVALID_ARGUMENT;
    }

    D3D11_TEXTURE2D_DESC input_desc {};
    input->GetDesc(&input_desc);
    if (input_desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
        input_desc.Width != instance->width || input_desc.Height != instance->height) {
      return FOUNDATION_DLSSNR_STATUS_INVALID_ARGUMENT;
    }
    D3D11_TEXTURE2D_DESC output_desc {};
    output->GetDesc(&output_desc);
    if (output_desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
        output_desc.Width != instance->width || output_desc.Height != instance->height) {
      return FOUNDATION_DLSSNR_STATUS_INVALID_ARGUMENT;
    }

    // The previous evaluate must be complete before the command allocator can
    // be reset; a single in-flight slot keeps the ordering explicit.
    if (!wait_cpu_fence(instance, instance->fence_value)) {
      return FOUNDATION_DLSSNR_STATUS_INTERNAL_ERROR;
    }
    collect_timing(instance);

    // 1) Publish this frame into the D3D12-visible mirror.
    instance->context11->CopyResource(instance->input_mirror11, input);
    if (instance->optical_flow && !instance->optical_flow->process(instance->first_frame)) {
      return FOUNDATION_DLSSNR_STATUS_RUNTIME_UNAVAILABLE;
    }
    const uint64_t input_ready = ++instance->fence_value;
    if (FAILED(instance->context11->Signal(instance->fence11, input_ready))) {
      return FOUNDATION_DLSSNR_STATUS_DEVICE_LOST;
    }
    instance->context11->Flush();
    if (FAILED(instance->queue12->Wait(instance->fence12, input_ready))) {
      return FOUNDATION_DLSSNR_STATUS_INTERNAL_ERROR;
    }

    // 2) Evaluate on D3D12.
    HRESULT hr = instance->allocator12->Reset();
    if (SUCCEEDED(hr)) hr = instance->list12->Reset(instance->allocator12, nullptr);
    if (FAILED(hr)) return FOUNDATION_DLSSNR_STATUS_INTERNAL_ERROR;

    const auto make_barrier = [&](ID3D12Resource *resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
      D3D12_RESOURCE_BARRIER barrier {};
      barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      barrier.Transition.pResource = resource;
      barrier.Transition.StateBefore = before;
      barrier.Transition.StateAfter = after;
      barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      return barrier;
    };
    D3D12_RESOURCE_BARRIER barriers[4];
    barriers[0] = make_barrier(instance->input_mirror12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barriers[1] = make_barrier(instance->output_mirror12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    barriers[2] = make_barrier(instance->motion12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barriers[3] = make_barrier(instance->zero_depth12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    instance->list12->ResourceBarrier(4, barriers);

    const NVSDK_NGX_Result param_result = ngx_invoke([&] {
      instance->parameters->Set(PARAM_COLOR, instance->input_mirror12);
      instance->parameters->Set(PARAM_OUTPUT, instance->output_mirror12);
      instance->parameters->Set(PARAM_MVEC, instance->motion12);
      instance->parameters->Set(PARAM_DEPTH, instance->zero_depth12);
      // Full-frame subrects for every resource.
      instance->parameters->Set("DLSSNR.ColorSubrectBaseX", 0u);
      instance->parameters->Set("DLSSNR.ColorSubrectBaseY", 0u);
      instance->parameters->Set("DLSSNR.ColorSubrectWidth", instance->width);
      instance->parameters->Set("DLSSNR.ColorSubrectHeight", instance->height);
      instance->parameters->Set("DLSSNR.OutputSubrectBaseX", 0u);
      instance->parameters->Set("DLSSNR.OutputSubrectBaseY", 0u);
      instance->parameters->Set("DLSSNR.OutputSubrectWidth", instance->width);
      instance->parameters->Set("DLSSNR.OutputSubrectHeight", instance->height);
      instance->parameters->Set("DLSSNR.MVecSubrectBaseX", 0u);
      instance->parameters->Set("DLSSNR.MVecSubrectBaseY", 0u);
      instance->parameters->Set("DLSSNR.MVecSubrectWidth", instance->width);
      instance->parameters->Set("DLSSNR.MVecSubrectHeight", instance->height);
      instance->parameters->Set("DLSSNR.DepthSubrectBaseX", 0u);
      instance->parameters->Set("DLSSNR.DepthSubrectBaseY", 0u);
      instance->parameters->Set("DLSSNR.DepthSubrectWidth", instance->width);
      instance->parameters->Set("DLSSNR.DepthSubrectHeight", instance->height);
      instance->parameters->Set(PARAM_MVEC_SCALE_X, 1.0f);
      instance->parameters->Set(PARAM_MVEC_SCALE_Y, 1.0f);
      instance->parameters->Set(PARAM_DEPTH_INVERTED, 1u);
      instance->parameters->Set(PARAM_INDICATOR_INVERT_X, 0u);
      instance->parameters->Set(PARAM_INDICATOR_INVERT_Y, 0u);
      instance->parameters->Set(PARAM_ENABLED, 1u);
      instance->parameters->Set(PARAM_RESET, instance->first_frame ? 1u : 0u);
      instance->parameters->Set(PARAM_STYLE, instance->config.style);
      instance->parameters->Set(PARAM_INTENSITY, instance->config.intensity);
      instance->parameters->Set(PARAM_LOCAL_TONE, instance->config.local_tone_strength);
      instance->parameters->Set(PARAM_LOCAL_STRUCTURE, instance->config.local_structure_strength);
      instance->parameters->Set(PARAM_SKIN_STRUCTURE, instance->config.skin_structure_strength);
      instance->parameters->Set(PARAM_AUTO_MASK, instance->config.auto_mask ? 1u : 0u);
      instance->parameters->Set(PARAM_UI_CORRECTION, instance->config.ui_correction ? 1u : 0u);
      return NVSDK_NGX_Result_Success;
    });
    if (!ngx_succeeded(param_result)) {
      // The command list is in the recording state; close it so the
      // allocator stays resettable if the instance is ever reused, and so
      // destroy does not release an open list (debug layer error).
      (void) instance->list12->Close();
      return FOUNDATION_DLSSNR_STATUS_INTERNAL_ERROR;
    }

    if (instance->timing_heap) instance->list12->EndQuery(instance->timing_heap, D3D12_QUERY_TYPE_TIMESTAMP, 0);
    const NVSDK_NGX_Result evaluate_result = ngx_invoke([&] {
      return instance->evaluate_feature(instance->list12, instance->feature, instance->parameters, nullptr);
    });
    if (!ngx_succeeded(evaluate_result)) {
      // Same recording-state concern as the parameter failure path above.
      (void) instance->list12->Close();
      return FOUNDATION_DLSSNR_STATUS_INTERNAL_ERROR;
    }
    if (instance->timing_heap) {
      instance->list12->EndQuery(instance->timing_heap, D3D12_QUERY_TYPE_TIMESTAMP, 1);
      instance->list12->ResolveQueryData(instance->timing_heap, D3D12_QUERY_TYPE_TIMESTAMP,
        0, 2, instance->timing_readback, 0);
    }

    D3D12_RESOURCE_BARRIER reverse[4];
    for (int i = 0; i < 4; ++i) {
      reverse[i] = barriers[i];
      D3D12_RESOURCE_TRANSITION_BARRIER &transition = reverse[i].Transition;
      const D3D12_RESOURCE_STATES before = transition.StateBefore;
      transition.StateBefore = transition.StateAfter;
      transition.StateAfter = before;
    }
    instance->list12->ResourceBarrier(4, reverse);
    if (FAILED(instance->list12->Close())) {
      return FOUNDATION_DLSSNR_STATUS_INTERNAL_ERROR;
    }
    instance->queue12->ExecuteCommandLists(1, reinterpret_cast<ID3D12CommandList *const *>(&instance->list12));
    const uint64_t output_ready = ++instance->fence_value;
    if (FAILED(instance->queue12->Signal(instance->fence12, output_ready))) {
      return FOUNDATION_DLSSNR_STATUS_DEVICE_LOST;
    }
    instance->timing_pending = instance->timing_heap != nullptr;

    // 3) D3D11 waits GPU-side, then copies the evaluated mirror. Evaluation
    // errors return above; the host filter retains the original capture frame.
    if (FAILED(instance->context11->Wait(instance->fence11, output_ready))) {
      return FOUNDATION_DLSSNR_STATUS_DEVICE_LOST;
    }
    instance->context11->CopyResource(output, instance->output_mirror11);
    instance->first_frame = false;
    return FOUNDATION_DLSSNR_STATUS_OK;
  }

  void
  adapter_flush(void *raw_instance) noexcept {
    auto *instance = static_cast<instance_t *>(raw_instance);
    if (!instance || !instance->queue12) return;
    const uint64_t value = ++instance->fence_value;
    if (!instance->fence12 || FAILED(instance->queue12->Signal(instance->fence12, value))) return;
    uint64_t completed = value;
    // Include the D3D11 output copy before releasing or reusing shared mirrors.
    if (instance->context11 && instance->fence11) {
      if (FAILED(instance->context11->Wait(instance->fence11, value))) return;
      completed = ++instance->fence_value;
      if (FAILED(instance->context11->Signal(instance->fence11, completed))) return;
      instance->context11->Flush();
    }
    if (wait_cpu_fence(instance, completed)) collect_timing(instance);
  }

}  // namespace

// ---------------------------------------------------------------------------
// C ABI exports.
// ---------------------------------------------------------------------------

namespace {

  foundation_dlssnr_status_e FOUNDATION_DLSSNR_CALL
  adapter_create_thunk(void *device, const foundation_dlssnr_config_t *config, void **out_instance) noexcept {
    try {
      return adapter_create(static_cast<ID3D11Device *>(device), config, out_instance);
    }
    catch (...) {
      return FOUNDATION_DLSSNR_STATUS_INTERNAL_ERROR;
    }
  }

  foundation_dlssnr_status_e FOUNDATION_DLSSNR_CALL
  adapter_process_thunk(void *instance, void *context, void *input, void *output) noexcept {
    try {
      return adapter_process(instance, context, input, output);
    }
    catch (...) {
      return FOUNDATION_DLSSNR_STATUS_INTERNAL_ERROR;
    }
  }

  void FOUNDATION_DLSSNR_CALL
  adapter_flush_thunk(void *instance) noexcept {
    try {
      adapter_flush(instance);
    }
    catch (...) {
    }
  }

  void FOUNDATION_DLSSNR_CALL
  adapter_destroy_thunk(void *instance) noexcept {
    try {
      adapter_destroy(instance);
    }
    catch (...) {
    }
  }

  const foundation_dlssnr_adapter_api_t g_dlssnr_adapter_api {
    FOUNDATION_DLSSNR_ADAPTER_ABI_VERSION,
    sizeof(foundation_dlssnr_adapter_api_t),
    &adapter_create_thunk,
    &adapter_process_thunk,
    &adapter_flush_thunk,
    &adapter_destroy_thunk,
  };

}  // namespace

extern "C" FOUNDATION_DLSSNR_EXPORT const foundation_dlssnr_adapter_api_t *FOUNDATION_DLSSNR_CALL
foundation_dlssnr_adapter_get_api(uint32_t requested_abi_version) {
  if (requested_abi_version != FOUNDATION_DLSSNR_ADAPTER_ABI_VERSION) {
    return nullptr;
  }
  return &g_dlssnr_adapter_api;
}
