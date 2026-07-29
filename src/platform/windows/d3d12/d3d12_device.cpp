/**
 * @file src/platform/windows/d3d12/d3d12_device.cpp
 * @brief D3D12 device, compute queue, shared fence, and ring bootstrap.
 */
#include "d3d12_device.h"

#include <limits>

namespace platf::dxgi::d3d12 {
  using Microsoft::WRL::ComPtr;

  namespace {
    constexpr DWORD self_test_timeout_ms = 2000;

    init_result_t
    failure(
      video_backend::fallback_reason_e reason,
      HRESULT hresult,
      std::string_view stage) {
      return { false, reason, hresult, stage };
    }

    HRESULT
    validate_same_adapter(
      IDXGIAdapter1 *adapter,
      ID3D11Device *d3d11_device) {
      DXGI_ADAPTER_DESC1 expected_desc {};
      auto status = adapter->GetDesc1(&expected_desc);
      if (FAILED(status)) {
        return status;
      }

      ComPtr<IDXGIDevice> dxgi_device;
      ComPtr<IDXGIAdapter> actual_adapter;
      ComPtr<IDXGIAdapter1> actual_adapter1;
      status = d3d11_device->QueryInterface(IID_PPV_ARGS(&dxgi_device));
      if (SUCCEEDED(status)) {
        status = dxgi_device->GetAdapter(&actual_adapter);
      }
      if (SUCCEEDED(status)) {
        status = actual_adapter.As(&actual_adapter1);
      }
      DXGI_ADAPTER_DESC1 actual_desc {};
      if (SUCCEEDED(status)) {
        status = actual_adapter1->GetDesc1(&actual_desc);
      }
      if (FAILED(status)) {
        return status;
      }

      return expected_desc.AdapterLuid.HighPart ==
                   actual_desc.AdapterLuid.HighPart &&
                 expected_desc.AdapterLuid.LowPart ==
                   actual_desc.AdapterLuid.LowPart ?
               S_OK :
               DXGI_ERROR_INVALID_CALL;
    }
  }  // namespace

  device_t::~device_t() {
    drain();
  }

  init_result_t
  device_t::initialize(
    IDXGIAdapter1 *adapter,
    ID3D11Device *d3d11_device,
    ID3D11DeviceContext *d3d11_context) {
    reset();
    if (!adapter || !d3d11_device || !d3d11_context) {
      return failure(
        video_backend::fallback_reason_e::d3d12_device_failed,
        E_INVALIDARG,
        "input_validation");
    }

    auto status = validate_same_adapter(adapter, d3d11_device);
    if (FAILED(status)) {
      return failure(
        video_backend::fallback_reason_e::d3d12_device_failed,
        status,
        "adapter_luid_validation");
    }

    status = D3D12CreateDevice(
      adapter,
      D3D_FEATURE_LEVEL_11_0,
      IID_PPV_ARGS(&device_));
    if (FAILED(status)) {
      return failure(
        video_backend::fallback_reason_e::d3d12_device_failed,
        status,
        "device_create");
    }

    status = probe_features();
    if (FAILED(status)) {
      reset();
      return failure(
        video_backend::fallback_reason_e::shader_model_unsupported,
        status,
        "feature_probe");
    }

    status = create_queue_fence_and_allocators();
    if (FAILED(status)) {
      reset();
      return failure(
        video_backend::fallback_reason_e::shared_fence_failed,
        status,
        "queue_fence_create");
    }

    status = self_test_shared_fence(d3d11_device, d3d11_context);
    if (FAILED(status)) {
      reset();
      return failure(
        video_backend::fallback_reason_e::shared_fence_failed,
        status,
        "shared_fence_self_test");
    }
    capabilities_.shared_fence = true;

    const auto rgba_status = self_test_shared_texture(
      d3d11_device,
      d3d11_context,
      DXGI_FORMAT_R16G16B16A16_FLOAT);
    const auto rgba_stage = self_test_stage_;
    capabilities_.rgba16f_bridge = SUCCEEDED(rgba_status);

    const auto nv12_status = self_test_shared_texture(
      d3d11_device,
      d3d11_context,
      DXGI_FORMAT_NV12);
    capabilities_.nv12_encoder_surface = SUCCEEDED(nv12_status);

    const auto p010_status = self_test_shared_texture(
      d3d11_device,
      d3d11_context,
      DXGI_FORMAT_P010);
    capabilities_.p010_encoder_surface = SUCCEEDED(p010_status);

    if (!capabilities_.has_viable_topology()) {
      const auto failed_capabilities = capabilities_;
      reset();
      auto result = failure(
        video_backend::fallback_reason_e::shared_resource_failed,
        rgba_status,
        rgba_stage);
      result.capabilities = failed_capabilities;
      return result;
    }

    if (!resource_ring_.begin_generation(1)) {
      reset();
      return failure(
        video_backend::fallback_reason_e::shared_fence_failed,
        E_UNEXPECTED,
        "ring_initialize");
    }

    available_ = true;
    return {
      true,
      video_backend::fallback_reason_e::none,
      S_OK,
      "ready",
      capabilities_,
    };
  }

  bool
  device_t::available() const {
    return available_;
  }

  ID3D12Device *
  device_t::device() const {
    return device_.Get();
  }

  ID3D12CommandQueue *
  device_t::compute_queue() const {
    return compute_queue_.Get();
  }

  ID3D12Fence *
  device_t::shared_fence() const {
    return shared_fence_.Get();
  }

  const capabilities_t &
  device_t::capabilities() const {
    return capabilities_;
  }

  std::uint64_t
  device_t::next_fence_value() {
    if (last_fence_value_ == std::numeric_limits<std::uint64_t>::max()) {
      return 0;
    }
    return ++last_fence_value_;
  }

  resource_ring_t &
  device_t::resource_ring() {
    return resource_ring_;
  }

  void
  device_t::drain() {
    (void) wait_idle();
    available_ = false;
  }

  HRESULT
  device_t::wait_idle() {
    if (!available_ || !compute_queue_ || !shared_fence_) {
      return E_UNEXPECTED;
    }
    const auto fence_value = next_fence_value();
    if (fence_value == 0) {
      return E_UNEXPECTED;
    }
    auto status =
      compute_queue_->Signal(shared_fence_.Get(), fence_value);
    if (FAILED(status)) {
      return status;
    }
    HANDLE completion_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!completion_event) {
      return HRESULT_FROM_WIN32(GetLastError());
    }
    status = shared_fence_->SetEventOnCompletion(
      fence_value,
      completion_event);
    if (SUCCEEDED(status) &&
        WaitForSingleObject(completion_event, self_test_timeout_ms) !=
          WAIT_OBJECT_0) {
      status = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }
    CloseHandle(completion_event);
    return status;
  }

  HRESULT
  device_t::probe_features() {
    D3D12_FEATURE_DATA_SHADER_MODEL shader_model {
      D3D_SHADER_MODEL_6_0,
    };
    auto status = device_->CheckFeatureSupport(
      D3D12_FEATURE_SHADER_MODEL,
      &shader_model,
      sizeof(shader_model));
    if (FAILED(status) || shader_model.HighestShaderModel < D3D_SHADER_MODEL_6_0) {
      return FAILED(status) ? status : E_NOINTERFACE;
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS1 options {};
    status = device_->CheckFeatureSupport(
      D3D12_FEATURE_D3D12_OPTIONS1,
      &options,
      sizeof(options));
    if (FAILED(status) || !options.WaveOps) {
      return FAILED(status) ? status : E_NOINTERFACE;
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS4 options4 {};
    status = device_->CheckFeatureSupport(
      D3D12_FEATURE_D3D12_OPTIONS4,
      &options4,
      sizeof(options4));
    if (FAILED(status)) {
      return status;
    }
    capabilities_.shared_resource_tier =
      options4.SharedResourceCompatibilityTier;
    return S_OK;
  }

  HRESULT
  device_t::create_queue_fence_and_allocators() {
    D3D12_COMMAND_QUEUE_DESC queue_desc {};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    auto status = device_->CreateCommandQueue(
      &queue_desc,
      IID_PPV_ARGS(&compute_queue_));
    if (FAILED(status)) {
      return status;
    }

    status = device_->CreateFence(
      0,
      D3D12_FENCE_FLAG_SHARED,
      IID_PPV_ARGS(&shared_fence_));
    if (FAILED(status)) {
      return status;
    }

    for (auto &allocator : command_allocators_) {
      status = device_->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_COMPUTE,
        IID_PPV_ARGS(&allocator));
      if (FAILED(status)) {
        return status;
      }
    }
    return S_OK;
  }

  HRESULT
  device_t::self_test_shared_fence(
    ID3D11Device *d3d11_device,
    ID3D11DeviceContext *d3d11_context) {
    HANDLE shared_handle = nullptr;
    auto status = device_->CreateSharedHandle(
      shared_fence_.Get(),
      nullptr,
      GENERIC_ALL,
      nullptr,
      &shared_handle);
    if (FAILED(status)) {
      return status;
    }

    ComPtr<ID3D11Device5> device5;
    ComPtr<ID3D11DeviceContext4> context4;
    ComPtr<ID3D11Fence> d3d11_fence;
    status = d3d11_device->QueryInterface(IID_PPV_ARGS(&device5));
    if (SUCCEEDED(status)) {
      status = d3d11_context->QueryInterface(IID_PPV_ARGS(&context4));
    }
    if (SUCCEEDED(status)) {
      status = device5->OpenSharedFence(
        shared_handle,
        IID_PPV_ARGS(&d3d11_fence));
    }
    CloseHandle(shared_handle);
    if (FAILED(status)) {
      return status;
    }

    const auto capture_ready = next_fence_value();
    const auto compute_done = next_fence_value();
    status = context4->Signal(d3d11_fence.Get(), capture_ready);
    if (SUCCEEDED(status)) {
      d3d11_context->Flush();
      status = compute_queue_->Wait(shared_fence_.Get(), capture_ready);
    }
    if (SUCCEEDED(status)) {
      status = compute_queue_->Signal(shared_fence_.Get(), compute_done);
    }
    if (FAILED(status)) {
      return status;
    }

    HANDLE completion_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!completion_event) {
      return HRESULT_FROM_WIN32(GetLastError());
    }
    status = shared_fence_->SetEventOnCompletion(
      compute_done,
      completion_event);
    if (SUCCEEDED(status) &&
        WaitForSingleObject(completion_event, self_test_timeout_ms) !=
          WAIT_OBJECT_0) {
      status = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }
    CloseHandle(completion_event);
    return status;
  }

  HRESULT
  device_t::self_test_shared_texture(
    ID3D11Device *d3d11_device,
    ID3D11DeviceContext *d3d11_context,
    DXGI_FORMAT format) {
    D3D12_HEAP_PROPERTIES heap_properties {};
    heap_properties.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC resource_desc {};
    resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resource_desc.Width = 64;
    resource_desc.Height = 64;
    resource_desc.DepthOrArraySize = 1;
    resource_desc.MipLevels = 1;
    resource_desc.Format = format;
    resource_desc.SampleDesc.Count = 1;
    resource_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resource_desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(
      D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
      D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS);

    ComPtr<ID3D12Resource> resource;
    self_test_stage_ = "shared_texture_create";
    auto status = device_->CreateCommittedResource(
      &heap_properties,
      D3D12_HEAP_FLAG_SHARED,
      &resource_desc,
      D3D12_RESOURCE_STATE_COMMON,
      nullptr,
      IID_PPV_ARGS(&resource));
    if (FAILED(status)) {
      return status;
    }

    HANDLE shared_handle = nullptr;
    self_test_stage_ = "shared_texture_handle";
    status = device_->CreateSharedHandle(
      resource.Get(),
      nullptr,
      GENERIC_ALL,
      nullptr,
      &shared_handle);
    if (FAILED(status)) {
      return status;
    }

    ComPtr<ID3D11Device1> device1;
    ComPtr<ID3D11Texture2D> texture;
    self_test_stage_ = "d3d11_device1_query";
    status = d3d11_device->QueryInterface(IID_PPV_ARGS(&device1));
    if (SUCCEEDED(status)) {
      self_test_stage_ = "d3d11_open_shared_texture";
      status = device1->OpenSharedResource1(
        shared_handle,
        IID_PPV_ARGS(&texture));
    }
    CloseHandle(shared_handle);
    if (FAILED(status)) {
      return status;
    }

    D3D11_TEXTURE2D_DESC source_desc {};
    source_desc.Width = 64;
    source_desc.Height = 64;
    source_desc.MipLevels = 1;
    source_desc.ArraySize = 1;
    source_desc.Format = format;
    source_desc.SampleDesc.Count = 1;
    source_desc.Usage = D3D11_USAGE_DEFAULT;
    ComPtr<ID3D11Texture2D> source;
    self_test_stage_ = "d3d11_copy_source_create";
    status = d3d11_device->CreateTexture2D(
      &source_desc,
      nullptr,
      &source);
    if (FAILED(status)) {
      return status;
    }

    self_test_stage_ = "d3d11_copy_to_shared_texture";
    d3d11_context->CopyResource(texture.Get(), source.Get());
    self_test_stage_ = "shared_texture_fence_round_trip";
    return self_test_shared_fence(d3d11_device, d3d11_context);
  }

  void
  device_t::reset() {
    available_ = false;
    for (auto &allocator : command_allocators_) {
      allocator.Reset();
    }
    shared_fence_.Reset();
    compute_queue_.Reset();
    device_.Reset();
    resource_ring_ = {};
    capabilities_ = {};
    last_fence_value_ = 0;
    self_test_stage_ = "none";
  }
}  // namespace platf::dxgi::d3d12
