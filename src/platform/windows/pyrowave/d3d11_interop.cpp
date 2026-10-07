/**
 * @file src/platform/windows/pyrowave/d3d11_interop.cpp
 * @brief Validation of D3D11 shared resources before PyroWave import.
 */
#include "d3d11_interop.h"

#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstring>

namespace pyrowave::windows {

  namespace {

    using Microsoft::WRL::ComPtr;

    [[nodiscard]] bool
    map_format(const DXGI_FORMAT format, image_format_e &mapped) noexcept {
      switch (format) {
      case DXGI_FORMAT_B8G8R8A8_UNORM:
      case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        mapped = image_format_e::bgra8_unorm;
        return true;
      case DXGI_FORMAT_R8G8B8A8_UNORM:
      case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        mapped = image_format_e::rgba8_unorm;
        return true;
      case DXGI_FORMAT_NV12:
        mapped = image_format_e::nv12;
        return true;
      case DXGI_FORMAT_P010:
        mapped = image_format_e::p010;
        return true;
      default:
        return false;
      }
    }

    [[nodiscard]] bool
    query_adapter_luid(ID3D11Device *device, LUID &luid, std::uint32_t &vendor_id, std::uint32_t &device_id) noexcept {
      ComPtr<IDXGIDevice> dxgi_device;
      if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi_device)))) {
        return false;
      }

      ComPtr<IDXGIAdapter> adapter;
      if (FAILED(dxgi_device->GetAdapter(&adapter))) {
        return false;
      }

      ComPtr<IDXGIAdapter1> adapter1;
      if (FAILED(adapter.As(&adapter1))) {
        return false;
      }

      DXGI_ADAPTER_DESC1 desc {};
      if (FAILED(adapter1->GetDesc1(&desc))) {
        return false;
      }

      luid = desc.AdapterLuid;
      vendor_id = desc.VendorId;
      device_id = desc.DeviceId;
      return true;
    }

    [[nodiscard]] bool
    can_export_shared_handle(ID3D11Texture2D *texture, const UINT misc_flags) noexcept {
      if ((misc_flags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE) != 0) {
        ComPtr<IDXGIResource1> resource;
        if (FAILED(texture->QueryInterface(IID_PPV_ARGS(&resource)))) {
          return false;
        }

        HANDLE handle = nullptr;
        const auto status = resource->CreateSharedHandle(
          nullptr,
          DXGI_SHARED_RESOURCE_READ,
          nullptr,
          &handle);
        if (FAILED(status) || handle == nullptr) {
          return false;
        }
        CloseHandle(handle);
        return true;
      }

      if ((misc_flags & (D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX)) != 0) {
        ComPtr<IDXGIResource> resource;
        HANDLE handle = nullptr;
        if (FAILED(texture->QueryInterface(IID_PPV_ARGS(&resource))) ||
            FAILED(resource->GetSharedHandle(&handle))) {
          return false;
        }
        return handle != nullptr;
      }

      return false;
    }

  }  // namespace

  interop_result_t
  inspect_d3d11_texture(
    ID3D11Device *device,
    ID3D11Texture2D *texture,
    d3d11_texture_info_t &result) noexcept {
    result = {};
    if (device == nullptr || texture == nullptr) {
      return { availability_e::failed, failure_e::invalid_state, sync_handle_kind_e::none };
    }

    D3D11_TEXTURE2D_DESC desc {};
    texture->GetDesc(&desc);
    if (desc.Width == 0 || desc.Height == 0 || desc.Width > 16384 || desc.Height > 16384 ||
        desc.Usage != D3D11_USAGE_DEFAULT || desc.CPUAccessFlags != 0 ||
        desc.MipLevels != 1 || desc.ArraySize != 1 || desc.SampleDesc.Count != 1) {
      return { availability_e::failed, failure_e::image_import_failed, sync_handle_kind_e::none };
    }
    image_format_e mapped_format {};
    if (!map_format(desc.Format, mapped_format)) {
      return { availability_e::failed, failure_e::format_conversion_failed, sync_handle_kind_e::none };
    }
    if ((mapped_format == image_format_e::nv12 || mapped_format == image_format_e::p010) &&
        ((desc.Width & 1u) != 0 || (desc.Height & 1u) != 0)) {
      return { availability_e::failed, failure_e::format_conversion_failed, sync_handle_kind_e::none };
    }

    if (!query_adapter_luid(device, result.adapter_luid, result.frame.device.vendor_id, result.frame.device.device_id)) {
      return { availability_e::failed, failure_e::device_mismatch, sync_handle_kind_e::none };
    }

    result.format = desc.Format;
    result.misc_flags = desc.MiscFlags;
    result.frame.width = desc.Width;
    result.frame.height = desc.Height;
    result.frame.format = mapped_format;
    result.frame.opaque_image = texture;
    result.frame.device.has_device_luid = true;
    std::memcpy(result.frame.device.device_luid.data(), &result.adapter_luid, sizeof(result.adapter_luid));

    result.keyed_mutex = (desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX) != 0;
    result.shared_handle = can_export_shared_handle(texture, desc.MiscFlags);
    if (!result.shared_handle) {
      return { availability_e::failed, failure_e::image_import_failed, sync_handle_kind_e::none };
    }

    if (result.keyed_mutex) {
      ComPtr<IDXGIKeyedMutex> keyed_mutex;
      if (FAILED(texture->QueryInterface(IID_PPV_ARGS(&keyed_mutex)))) {
        return { availability_e::failed, failure_e::synchronization_unsupported, sync_handle_kind_e::none };
      }
    }

    const auto sync_kind = result.keyed_mutex
      ? sync_handle_kind_e::win32_kmt
      : sync_handle_kind_e::win32_opaque;
    return { availability_e::available, failure_e::none, sync_kind };
  }

  bool
  matches_adapter(const d3d11_texture_info_t &texture, const device_identity_t &expected) noexcept {
    if (!expected.has_device_luid || !texture.frame.device.has_device_luid) {
      return false;
    }

    return std::memcmp(texture.frame.device.device_luid.data(), expected.device_luid.data(), expected.device_luid.size()) == 0;
  }

}  // namespace pyrowave::windows
