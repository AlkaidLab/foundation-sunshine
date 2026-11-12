/**
 * @file src/platform/windows/ds5/ds5_pad_audio.cpp
 * @brief See ds5_pad_audio.h.
 *
 * Instance and container identity: Microsoft, "Device instance IDs" and
 * "Container IDs" (Windows Hardware Developer documentation), read 2026-11-07.
 * Mute: USB Device Class Definition for Audio Devices release 1.0, feature
 * unit mute control, and IAudioEndpointVolume (Windows SDK endpointvolume.h).
 */
#include "src/platform/windows/ds5/ds5_pad_audio.h"

#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
  #define NOMINMAX
#endif
#include <windows.h>

#include <mmdeviceapi.h>

#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <mutex>
#include <string_view>

namespace platf::ds5::pad_audio {
  namespace {
    constexpr const wchar_t *kMarker = L"Wireless Controller";
    constexpr int kLeaseCount = 8;

    struct leases_t {
      std::mutex mutex;
      int owner[kLeaseCount];
      leases_t() {
        for (int index = 0; index < kLeaseCount; ++index) owner[index] = -1;
      }
    };

    leases_t &
    leases() {
      static leases_t value;
      return value;
    }

    struct com_scope {
      HRESULT status;
      com_scope():
          status(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
      ~com_scope() {
        if (SUCCEEDED(status)) CoUninitialize();
      }
      bool
      usable() const {
        return SUCCEEDED(status) || status == RPC_E_CHANGED_MODE;
      }
    };

    bool
    contains_marker(const wchar_t *name) {
      if (name == nullptr) return false;
      const auto haystack = std::wstring_view(name);
      const auto needle = std::wstring_view(kMarker);
      return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(), [](wchar_t left, wchar_t right) {
        return std::towupper(left) == std::towupper(right);
      }) != haystack.end();
    }

    IMMDevice *
    active_capture() {
      IMMDeviceEnumerator *enumerator = nullptr;
      if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&enumerator))) ||
          enumerator == nullptr) {
        return nullptr;
      }
      IMMDeviceCollection *collection = nullptr;
      IMMDevice *found = nullptr;
      if (SUCCEEDED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection)) && collection != nullptr) {
        UINT count = 0;
        collection->GetCount(&count);
        for (UINT index = 0; index < count && found == nullptr; ++index) {
          IMMDevice *device = nullptr;
          if (FAILED(collection->Item(index, &device)) || device == nullptr) continue;
          IPropertyStore *store = nullptr;
          bool ours = false;
          if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) && store != nullptr) {
            PROPVARIANT value;
            PropVariantInit(&value);
            if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR) {
              ours = contains_marker(value.pwszVal);
            }
            PropVariantClear(&value);
            store->Release();
          }
          if (ours)
            found = device;
          else
            device->Release();
        }
        collection->Release();
      }
      enumerator->Release();
      return found;
    }

    std::optional<bool>
    read_mute(IMMDevice *device) {
      if (device == nullptr) return std::nullopt;
      IAudioEndpointVolume *volume = nullptr;
      if (FAILED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(&volume))) || volume == nullptr) {
        return std::nullopt;
      }
      BOOL muted = FALSE;
      const auto status = volume->GetMute(&muted);
      volume->Release();
      if (FAILED(status)) return std::nullopt;
      return muted != FALSE;
    }

    std::optional<bool>
    write_mute(IMMDevice *device, bool muted) {
      if (device == nullptr) return std::nullopt;
      IAudioEndpointVolume *volume = nullptr;
      if (FAILED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(&volume))) || volume == nullptr) {
        return std::nullopt;
      }
      const auto status = volume->SetMute(muted ? TRUE : FALSE, nullptr);
      BOOL read_back = FALSE;
      const auto read_status = volume->GetMute(&read_back);
      volume->Release();
      if (FAILED(status) || FAILED(read_status)) return std::nullopt;
      return read_back != FALSE;
    }
  }  // namespace

  std::wstring
  lease_serial(int global_index) {
    auto &table = leases();
    std::lock_guard lock(table.mutex);
    int number = -1;
    for (int index = 0; index < kLeaseCount; ++index) {
      if (table.owner[index] == global_index) number = index + 1;
    }
    if (number < 0) {
      for (int index = 0; index < kLeaseCount; ++index) {
        if (table.owner[index] < 0) {
          table.owner[index] = global_index;
          number = index + 1;
          break;
        }
      }
    }
    if (number < 0) number = 1;
    wchar_t text[16] {};
    std::swprintf(text, 16, L"02DA55%06X", static_cast<unsigned>(number));
    return text;
  }

  void
  release_serial(int global_index) {
    auto &table = leases();
    std::lock_guard lock(table.mutex);
    for (int index = 0; index < kLeaseCount; ++index) {
      if (table.owner[index] == global_index) table.owner[index] = -1;
    }
  }

  std::array<std::uint8_t, 6>
  mac_from_serial(const std::wstring &serial) {
    std::array<std::uint8_t, 6> mac {};
    if (serial.size() < 12) return mac;
    auto nibble = [](wchar_t value) -> int {
      if (value >= L'0' && value <= L'9') return static_cast<int>(value - L'0');
      if (value >= L'a' && value <= L'f') return static_cast<int>(value - L'a' + 10);
      if (value >= L'A' && value <= L'F') return static_cast<int>(value - L'A' + 10);
      return -1;
    };
    for (std::size_t index = 0; index < 6; ++index) {
      const int high = nibble(serial[index * 2]);
      const int low = nibble(serial[index * 2 + 1]);
      if (high < 0 || low < 0) return {};
      mac[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return mac;
  }

  std::optional<bool>
  capture_muted() {
    com_scope com;
    if (!com.usable()) return std::nullopt;
    IMMDevice *device = active_capture();
    const auto muted = read_mute(device);
    if (device != nullptr) device->Release();
    return muted;
  }

  std::optional<bool>
  set_capture_mute(bool muted) {
    com_scope com;
    if (!com.usable()) return std::nullopt;
    IMMDevice *device = active_capture();
    const auto applied = write_mute(device, muted);
    if (device != nullptr) device->Release();
    return applied;
  }

  std::optional<bool>
  toggle_capture_mute() {
    com_scope com;
    if (!com.usable()) return std::nullopt;
    IMMDevice *device = active_capture();
    const auto current = read_mute(device);
    std::optional<bool> applied;
    if (current.has_value()) applied = write_mute(device, !*current);
    if (device != nullptr) device->Release();
    return applied;
  }
}  // namespace platf::ds5::pad_audio
