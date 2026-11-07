/**
 * @file src/platform/windows/audio_default_guard.cpp
 * @brief Record and restore the default audio endpoints around a virtual device.
 */
#include "audio_default_guard.h"

#include <audioclient.h>
#include <endpointvolume.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <propidl.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmreg.h>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <sstream>
#include <thread>

// Must be the last included file
// clang-format off
#include "PolicyConfig.h"
// clang-format on

namespace platf::audio {

  namespace {
    const GUID &
    policy_config_clsid() {
#ifdef __MINGW32__
      return CLSID_CPolicyConfigClient;
#else
      return __uuidof(CPolicyConfigClient);
#endif
    }

    const GUID &
    policy_config_iid() {
#ifdef __MINGW32__
      return IID_IPolicyConfig;
#else
      return __uuidof(IPolicyConfig);
#endif
    }

    /* PKEY_DeviceInterface_FriendlyName: the name of the adapter (for USB audio, the product string). */
    const PROPERTYKEY kAdapterFriendlyName = { { 0x026e516e, 0xb814, 0x414b, { 0x83, 0xcd, 0x85, 0x6d, 0x6f, 0xef, 0x48, 0x22 } }, 2 };

    bool
    contains_ignoring_case(std::wstring_view haystack, std::wstring_view needle) {
      if (needle.empty()) return false;
      return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(), [](wchar_t a, wchar_t b) {
        return std::towupper(a) == std::towupper(b);
      }) != haystack.end();
    }

    /** Joins the apartment of the calling thread for the scope, or tolerates one that exists. */
    struct com_scope {
      HRESULT status;
      com_scope():
          status(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
      ~com_scope() {
        if (SUCCEEDED(status)) CoUninitialize();
      }
      bool
      usable() const { return SUCCEEDED(status) || status == RPC_E_CHANGED_MODE; }
    };
  }  // namespace

  std::optional<std::vector<default_endpoint>>
  capture_default_endpoints() {
    com_scope com;
    if (!com.usable()) return std::nullopt;

    IMMDeviceEnumerator *enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
          __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&enumerator)))) {
      return std::nullopt;
    }

    std::vector<default_endpoint> result;
    for (const auto flow : { eRender, eCapture }) {
      for (const auto role : { eConsole, eMultimedia, eCommunications }) {
        IMMDevice *device = nullptr;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(flow, role, &device)) && device) {
          LPWSTR raw_id = nullptr;
          if (SUCCEEDED(device->GetId(&raw_id)) && raw_id) {
            result.push_back({ flow, role, raw_id });
            CoTaskMemFree(raw_id);
          }
          device->Release();
        }
      }
    }
    enumerator->Release();
    return result;
  }

  std::set<std::wstring>
  capture_active_endpoint_ids() {
    std::set<std::wstring> ids;
    com_scope com;
    if (!com.usable()) return ids;
    IMMDeviceEnumerator *enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
          __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&enumerator)))) {
      return ids;
    }
    for (const auto flow : { eRender, eCapture }) {
      IMMDeviceCollection *collection = nullptr;
      if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection)) || !collection) continue;
      UINT count = 0;
      collection->GetCount(&count);
      for (UINT index = 0; index != count; ++index) {
        IMMDevice *device = nullptr;
        if (FAILED(collection->Item(index, &device)) || !device) continue;
        LPWSTR raw_id = nullptr;
        if (SUCCEEDED(device->GetId(&raw_id)) && raw_id) {
          ids.insert(raw_id);
          CoTaskMemFree(raw_id);
        }
        device->Release();
      }
      collection->Release();
    }
    enumerator->Release();
    return ids;
  }

  bool
  restore_default_endpoints(const std::vector<default_endpoint> &original, std::initializer_list<std::wstring_view> markers,
    const std::set<std::wstring> *existing) {
    if (original.empty()) return false;
    com_scope com;
    if (!com.usable()) return false;

    IMMDeviceEnumerator *enumerator = nullptr;
    IPolicyConfig *policy = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
          __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&enumerator))) ||
        FAILED(CoCreateInstance(policy_config_clsid(), nullptr, CLSCTX_ALL,
          policy_config_iid(), reinterpret_cast<void **>(&policy)))) {
      if (enumerator) enumerator->Release();
      if (policy) policy->Release();
      return false;
    }

    bool changed = false;
    for (const auto &saved : original) {
      IMMDevice *current = nullptr;
      if (FAILED(enumerator->GetDefaultAudioEndpoint(saved.flow, saved.role, &current)) || !current) {
        continue;
      }
      LPWSTR current_id = nullptr;
      const auto got_id = SUCCEEDED(current->GetId(&current_id)) && current_id;
      const bool is_same = got_id && saved.id == current_id;
      const bool pre_existing = got_id && existing && existing->count(current_id) != 0;
      if (current_id) CoTaskMemFree(current_id);

      bool is_ours = false;
      IPropertyStore *store = nullptr;
      if (SUCCEEDED(current->OpenPropertyStore(STGM_READ, &store)) && store) {
        for (const PROPERTYKEY *key : { &PKEY_Device_InstanceId, &PKEY_Device_FriendlyName, &kAdapterFriendlyName }) {
          PROPVARIANT value;
          PropVariantInit(&value);
          if (SUCCEEDED(store->GetValue(*key, &value)) && value.vt == VT_LPWSTR && value.pwszVal) {
            for (const auto marker : markers) is_ours = is_ours || contains_ignoring_case(value.pwszVal, marker);
          }
          PropVariantClear(&value);
        }
      }
      if (store) store->Release();
      current->Release();
      if (is_same || pre_existing || !is_ours) continue;
      if (SUCCEEDED(policy->SetDefaultEndpoint(saved.id.c_str(), saved.role))) changed = true;
    }

    policy->Release();
    enumerator->Release();
    return changed;
  }

  void
  restore_default_endpoints_after_attach(const std::vector<default_endpoint> &original, std::initializer_list<std::wstring_view> markers,
    std::initializer_list<std::chrono::milliseconds> delays, const std::set<std::wstring> *existing) {
    /* The class driver publishes the endpoints after the attach has completed.
     * The restore is conditional on the default being our device, so a
     * selection the user makes during the window is left alone. */
    for (const auto delay : delays) {
      std::this_thread::sleep_for(delay);
      (void) restore_default_endpoints(original, markers, existing);
    }
  }

}  // namespace platf::audio

namespace platf::audio {
  namespace {
    /* {1da5d803-d492-4edd-8c23-e0c0ffee7f0e}: the endpoint's own properties. 3 is the physical speaker mask, 6 the full range mask. */
    const PROPERTYKEY kPhysicalSpeakers = { { 0x1da5d803, 0xd492, 0x4edd, { 0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e } }, 3 };
    const PROPERTYKEY kFullRangeSpeakers = { { 0x1da5d803, 0xd492, 0x4edd, { 0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e } }, 6 };
    /* PKEY_AudioEngine_DeviceFormat: the shared-mode format of the endpoint. */
    const PROPERTYKEY kDeviceFormat = { { 0xf19f064d, 0x082c, 0x4e27, { 0xbc, 0x73, 0x68, 0x82, 0xa1, 0xbb, 0x8e, 0x4c } }, 0 };
    constexpr DWORD kQuadMask = 0x33;  // front left and right, back left and right

    bool
    format_is_quad(const PROPVARIANT &value) {
      if (value.vt != VT_BLOB || value.blob.cbSize < sizeof(WAVEFORMATEX) || !value.blob.pBlobData) return false;
      const auto *format = reinterpret_cast<const WAVEFORMATEX *>(value.blob.pBlobData);
      if (format->nChannels != 4 || format->nSamplesPerSec != 48000 || format->wBitsPerSample != 16) return false;
      if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && value.blob.cbSize >= sizeof(WAVEFORMATEXTENSIBLE)) {
        return reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(format)->dwChannelMask == kQuadMask;
      }
      return format->wFormatTag == WAVE_FORMAT_PCM;
    }

    DWORD
    read_uint(IPropertyStore *store, const PROPERTYKEY &key) {
      PROPVARIANT value;
      PropVariantInit(&value);
      DWORD result = 0;
      if (SUCCEEDED(store->GetValue(key, &value)) && value.vt == VT_UI4) result = value.ulVal;
      PropVariantClear(&value);
      return result;
    }

    bool
    read_format_ok(IPropertyStore *store) {
      PROPVARIANT value;
      PropVariantInit(&value);
      bool ok = SUCCEEDED(store->GetValue(kDeviceFormat, &value)) && format_is_quad(value);
      PropVariantClear(&value);
      return ok;
    }

    std::string
    narrow_text(const std::wstring &text) {
      const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
      std::string out(bytes > 0 ? static_cast<std::size_t>(bytes) : 0u, '\0');
      if (bytes > 0) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), bytes, nullptr, nullptr);
      return out;
    }

    std::set<std::wstring>
    read_known(const std::filesystem::path &file) {
      std::set<std::wstring> known;
      std::ifstream in(file);
      std::string line;
      while (std::getline(in, line)) {
        if (line.empty()) continue;
        const int chars = MultiByteToWideChar(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), nullptr, 0);
        std::wstring wide(chars > 0 ? static_cast<std::size_t>(chars) : 0u, L'\0');
        if (chars > 0) MultiByteToWideChar(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), wide.data(), chars);
        known.insert(wide);
      }
      return known;
    }
  }  // namespace

  std::vector<pad_endpoint_state_t>
  configure_pad_endpoints(std::wstring_view marker, const std::filesystem::path &known_file) {
    std::vector<pad_endpoint_state_t> results;
    com_scope com;
    if (!com.usable()) return results;
    IMMDeviceEnumerator *enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
          __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&enumerator)))) {
      return results;
    }
    auto known = read_known(known_file);
    std::vector<std::wstring> newly_known;
    for (const auto flow : { eRender, eCapture }) {
      IMMDeviceCollection *collection = nullptr;
      if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection)) || !collection) continue;
      UINT count = 0;
      collection->GetCount(&count);
      for (UINT index = 0; index != count; ++index) {
        IMMDevice *device = nullptr;
        if (FAILED(collection->Item(index, &device)) || !device) continue;
        pad_endpoint_state_t state;
        state.flow = flow;
        bool ours = false;
        if (LPWSTR raw_id = nullptr; SUCCEEDED(device->GetId(&raw_id)) && raw_id) {
          state.id = raw_id;
          CoTaskMemFree(raw_id);
        }
        if (IPropertyStore *store = nullptr; SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) && store) {
          for (const PROPERTYKEY *key : { &PKEY_Device_FriendlyName, &kAdapterFriendlyName }) {
            PROPVARIANT value;
            PropVariantInit(&value);
            if (SUCCEEDED(store->GetValue(*key, &value)) && value.vt == VT_LPWSTR && value.pwszVal) {
              if (key == &PKEY_Device_FriendlyName) state.name = value.pwszVal;
              ours = ours || contains_ignoring_case(value.pwszVal, marker);
            }
            PropVariantClear(&value);
          }
          store->Release();
        }
        if (!ours) {
          device->Release();
          continue;
        }
        state.first_time = known.find(state.id) == known.end();

        if (flow == eRender) {
          if (IPropertyStore *store = nullptr; SUCCEEDED(device->OpenPropertyStore(STGM_READWRITE, &store)) && store) {
            state.speakers_before = read_uint(store, kPhysicalSpeakers);
            state.full_range_before = read_uint(store, kFullRangeSpeakers);
            state.format_before_ok = read_format_ok(store);
            bool wrote = false;
            if (state.speakers_before != kQuadMask) {
              PROPVARIANT value;
              PropVariantInit(&value);
              value.vt = VT_UI4;
              value.ulVal = kQuadMask;
              wrote = SUCCEEDED(store->SetValue(kPhysicalSpeakers, value)) || wrote;
            }
            if (state.full_range_before != kQuadMask) {
              PROPVARIANT value;
              PropVariantInit(&value);
              value.vt = VT_UI4;
              value.ulVal = kQuadMask;
              wrote = SUCCEEDED(store->SetValue(kFullRangeSpeakers, value)) || wrote;
            }
            if (!state.format_before_ok) {
              WAVEFORMATEXTENSIBLE format {};
              format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
              format.Format.nChannels = 4;
              format.Format.nSamplesPerSec = 48000;
              format.Format.wBitsPerSample = 16;
              format.Format.nBlockAlign = 8;
              format.Format.nAvgBytesPerSec = 48000 * 8;
              format.Format.cbSize = 22;
              format.Samples.wValidBitsPerSample = 16;
              format.dwChannelMask = kQuadMask;
              format.SubFormat = { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };  // KSDATAFORMAT_SUBTYPE_PCM
              PROPVARIANT value;
              PropVariantInit(&value);
              value.vt = VT_BLOB;
              value.blob.cbSize = sizeof(format);
              value.blob.pBlobData = reinterpret_cast<BYTE *>(&format);
              wrote = SUCCEEDED(store->SetValue(kDeviceFormat, value)) || wrote;
            }
            if (wrote) {
              store->Commit();
              state.configured_now = true;
            }
            store->Release();
          }
          if (IPropertyStore *store = nullptr; SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) && store) {
            state.speakers_after = read_uint(store, kPhysicalSpeakers);
            state.full_range_after = read_uint(store, kFullRangeSpeakers);
            state.format_after_ok = read_format_ok(store);
            store->Release();
          }
        }

        IAudioEndpointVolume *volume = nullptr;
        if (SUCCEEDED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(&volume))) && volume) {
          BOOL muted = FALSE;
          float level = -1.0f;
          volume->GetMasterVolumeLevelScalar(&level);
          volume->GetMute(&muted);
          state.volume_before = level;
          state.mute_before = muted != FALSE;
          if (state.first_time) {
            if (level < 0.999f) volume->SetMasterVolumeLevelScalar(1.0f, nullptr);
            if (muted) volume->SetMute(FALSE, nullptr);
            state.volume_written = level < 0.999f || muted;
            state.configured_now = state.configured_now || state.volume_written;
          }
          else if (level <= 0.001f || muted) {
            state.warning = "known endpoint is " + std::string(muted ? "muted" : "at volume 0") +
                            ": audio and haptics through it are silent until the volume is raised (not changed here)";
          }
          volume->GetMasterVolumeLevelScalar(&level);
          volume->GetMute(&muted);
          state.volume_after = level;
          state.mute_after = muted != FALSE;
          volume->Release();
        }
        state.verified = state.volume_after > 0.99f && !state.mute_after &&
                         (flow == eCapture || (state.speakers_after == kQuadMask && state.full_range_after == kQuadMask && state.format_after_ok));
        if (!state.first_time && !state.verified) {
          // A known endpoint the user lowered is not a failure of the setup; only the speaker configuration is required.
          state.verified = flow == eCapture || (state.speakers_after == kQuadMask && state.full_range_after == kQuadMask && state.format_after_ok);
        }
        if (state.first_time && state.verified) newly_known.push_back(state.id);
        results.push_back(std::move(state));
        device->Release();
      }
      collection->Release();
    }
    enumerator->Release();
    if (!newly_known.empty()) {
      std::error_code ignored;
      std::filesystem::create_directories(known_file.parent_path(), ignored);
      std::ofstream out(known_file, std::ios::app);
      for (const auto &id : newly_known) out << narrow_text(id) << '\n';
    }
    return results;
  }

  std::vector<std::wstring>
  list_stale_pad_endpoints(std::wstring_view marker) {
    std::vector<std::wstring> stale;
    com_scope com;
    if (!com.usable()) return stale;
    IMMDeviceEnumerator *enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
          __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&enumerator)))) {
      return stale;
    }
    for (const auto flow : { eRender, eCapture }) {
      IMMDeviceCollection *collection = nullptr;
      if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_NOTPRESENT | DEVICE_STATE_UNPLUGGED | DEVICE_STATE_DISABLED, &collection)) || !collection) continue;
      UINT count = 0;
      collection->GetCount(&count);
      for (UINT index = 0; index != count; ++index) {
        IMMDevice *device = nullptr;
        if (FAILED(collection->Item(index, &device)) || !device) continue;
        if (IPropertyStore *store = nullptr; SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) && store) {
          PROPVARIANT value;
          PropVariantInit(&value);
          if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR && value.pwszVal &&
              contains_ignoring_case(value.pwszVal, marker)) {
            stale.push_back(std::wstring(flow == eRender ? L"render: " : L"capture: ") + value.pwszVal);
          }
          PropVariantClear(&value);
          store->Release();
        }
        device->Release();
      }
      collection->Release();
    }
    enumerator->Release();
    return stale;
  }

  std::vector<std::wstring>
  prune_phantom_pad_devices(std::wstring_view hardware_marker, std::wstring_view keep_marker) {
    std::vector<std::wstring> removed;
    const auto instance_id = [](HDEVINFO set, SP_DEVINFO_DATA &data) {
      wchar_t id[512] {};
      SetupDiGetDeviceInstanceIdW(set, &data, id, 512, nullptr);
      return std::wstring(id);
    };
    std::set<std::wstring> present;
    if (HDEVINFO now = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT); now != INVALID_HANDLE_VALUE) {
      SP_DEVINFO_DATA data {};
      data.cbSize = sizeof data;
      for (DWORD i = 0; SetupDiEnumDeviceInfo(now, i, &data); ++i) present.insert(instance_id(now, data));
      SetupDiDestroyDeviceInfoList(now);
    }
    HDEVINFO all = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_ALLCLASSES);
    if (all == INVALID_HANDLE_VALUE) return removed;
    SP_DEVINFO_DATA data {};
    data.cbSize = sizeof data;
    for (DWORD i = 0; SetupDiEnumDeviceInfo(all, i, &data); ++i) {
      const auto id = instance_id(all, data);
      if (!contains_ignoring_case(id, hardware_marker) || contains_ignoring_case(id, L"&MI_") ||
          contains_ignoring_case(id, keep_marker) || present.count(id) != 0) {
        continue;
      }
      // Only the top-level composite device (USB\VID_xxxx&PID_xxxx\<instance>) of an earlier, serial-less attach.
      if (SetupDiCallClassInstaller(DIF_REMOVE, all, &data)) removed.push_back(id);
    }
    SetupDiDestroyDeviceInfoList(all);
    return removed;
  }
}  // namespace platf::audio
