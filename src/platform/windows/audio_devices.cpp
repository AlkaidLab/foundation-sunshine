/**
 * @file src/platform/windows/audio_devices.cpp
 * @brief Reads the audio endpoints of the host through the MMDevice API.
 *
 * Sources (kept in docs/authority/source-index.tsv):
 *  - IMMDeviceEnumerator::EnumAudioEndpoints: the endpoints of one direction in the states asked for.
 *  - IMMDeviceEnumerator::GetDefaultAudioEndpoint: the default endpoint of a direction and role.
 *  - DEVICE_STATE_XXX constants and EndpointFormFactor: what the state and form factor values mean.
 * Nothing here opens an endpoint for audio, changes a default or writes a property: it only reads.
 */
#include <windows.h>

#include <cfgmgr32.h>
#include <mmdeviceapi.h>
#include <objbase.h>
#include <propidl.h>
#include <setupapi.h>

#include <algorithm>
#include <string>

#include "src/audio_devices.h"
#include "src/platform/windows/misc.h"

namespace audio_devices {
  namespace {
    // The property keys by value: mingw's mmdeviceapi.h declares PKEY_AudioEndpoint_FormFactor only extern, so
    // naming it here would need INITGUID in this file, which would define every GUID of the headers a second time.
    constexpr PROPERTYKEY key_device_desc = { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 2 };
    constexpr PROPERTYKEY key_friendly_name = { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };
    constexpr PROPERTYKEY key_adapter_name = { { 0x026e516e, 0xb814, 0x414b, { 0x83, 0xcd, 0x85, 0x6d, 0x6f, 0xef, 0x48, 0x22 } }, 2 };
    constexpr PROPERTYKEY key_form_factor = { { 0x1da5d803, 0xd492, 0x4edd, { 0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e } }, 0 };

    // the setup class of sound, video and game controllers (GUID_DEVCLASS_MEDIA): the class of the VB-CABLE device node
    constexpr GUID class_media = { 0x4d36e96c, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 } };

    // CLSID_MMDeviceEnumerator and IID_IMMDeviceEnumerator are in the headers, only declared: spelled out here for the same reason.
    constexpr CLSID clsid_enumerator = { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
    constexpr IID iid_enumerator = { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };

    template<class T>
    struct com_ptr_t {
      T *p = nullptr;
      com_ptr_t() = default;
      com_ptr_t(const com_ptr_t &) = delete;
      com_ptr_t &
      operator=(const com_ptr_t &) = delete;
      ~com_ptr_t() {
        if (p) p->Release();
      }
      T **
      out() {
        return &p;
      }
      T *
      operator->() const {
        return p;
      }
      explicit
      operator bool() const {
        return p != nullptr;
      }
    };

    struct prop_variant_t {
      PROPVARIANT value;
      prop_variant_t() {
        PropVariantInit(&value);
      }
      ~prop_variant_t() {
        PropVariantClear(&value);
      }
      prop_variant_t(const prop_variant_t &) = delete;
      prop_variant_t &
      operator=(const prop_variant_t &) = delete;
    };

    std::string
    read_string(IPropertyStore *store, const PROPERTYKEY &key) {
      prop_variant_t variant;
      if (FAILED(store->GetValue(key, &variant.value)) || variant.value.vt != VT_LPWSTR || !variant.value.pwszVal) return {};
      return platf::to_utf8(std::wstring(variant.value.pwszVal));
    }

    std::string
    default_id(IMMDeviceEnumerator *enumerator, EDataFlow flow, ERole role) {
      com_ptr_t<IMMDevice> device;
      if (FAILED(enumerator->GetDefaultAudioEndpoint(flow, role, device.out())) || !device) return {};
      LPWSTR id = nullptr;
      if (FAILED(device->GetId(&id)) || !id) return {};
      std::string out = platf::to_utf8(std::wstring(id));
      CoTaskMemFree(id);
      return out;
    }

    void
    read_flow(IMMDeviceEnumerator *enumerator, EDataFlow flow, std::vector<endpoint_t> &out) {
      const auto flow_value = flow == eRender ? flow_e::render : flow_e::capture;
      const auto default_console = default_id(enumerator, flow, eConsole);
      const auto default_comms = default_id(enumerator, flow, eCommunications);

      com_ptr_t<IMMDeviceCollection> collection;
      if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATEMASK_ALL, collection.out())) || !collection) return;
      UINT count = 0;
      collection->GetCount(&count);
      for (UINT i = 0; i < count; ++i) {
        com_ptr_t<IMMDevice> device;
        if (FAILED(collection->Item(i, device.out())) || !device) continue;

        endpoint_t endpoint;
        endpoint.flow = flow_value;
        LPWSTR id = nullptr;
        if (FAILED(device->GetId(&id)) || !id) continue;
        endpoint.id = platf::to_utf8(std::wstring(id));
        CoTaskMemFree(id);

        DWORD state = 0;
        device->GetState(&state);
        endpoint.state = std::string(state_name(state));
        if (endpoint.state.empty()) continue;

        com_ptr_t<IPropertyStore> store;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, store.out())) && store) {
          endpoint.name = read_string(store.p, key_friendly_name);
          endpoint.adapter = read_string(store.p, key_adapter_name);
          endpoint.description = read_string(store.p, key_device_desc);
          prop_variant_t form;
          int form_factor = -1;
          if (SUCCEEDED(store->GetValue(key_form_factor, &form.value)) && form.value.vt == VT_UI4) form_factor = static_cast<int>(form.value.ulVal);
          endpoint.form_factor = std::string(form_factor_name(form_factor));
        }
        else {
          endpoint.form_factor = "unknown";
        }
        endpoint.is_default = endpoint.id == default_console;
        endpoint.is_default_communications = endpoint.id == default_comms;
        out.push_back(std::move(endpoint));
      }
    }
  }  // namespace

  bool
  enumerate_vb_cable_drivers(std::vector<driver_t> &out) {
    out.clear();
    // DIGCF_PRESENT lists the devices that are in the device tree, a disabled one included (it shows with a problem code, not as absent)
    HDEVINFO set = SetupDiGetClassDevsW(&class_media, nullptr, nullptr, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return false;
    SP_DEVINFO_DATA info {};
    info.cbSize = sizeof(info);
    for (DWORD index = 0; SetupDiEnumDeviceInfo(set, index, &info); ++index) {
      auto read = [&](DWORD property) {
        wchar_t buffer[256] = {};
        DWORD type = 0;
        if (!SetupDiGetDeviceRegistryPropertyW(set, &info, property, &type, reinterpret_cast<PBYTE>(buffer), sizeof(buffer) - sizeof(wchar_t), nullptr) || type != REG_SZ) return std::string();
        return platf::to_utf8(std::wstring(buffer));
      };
      driver_t driver;
      driver.name = read(SPDRP_FRIENDLYNAME);
      if (driver.name.empty()) driver.name = read(SPDRP_DEVICEDESC);
      if (!is_vb_cable_driver(driver)) continue;
      wchar_t instance[MAX_DEVICE_ID_LEN] = {};
      if (SetupDiGetDeviceInstanceIdW(set, &info, instance, MAX_DEVICE_ID_LEN, nullptr)) driver.instance_id = platf::to_utf8(std::wstring(instance));
      ULONG status = 0;
      ULONG problem = 0;
      if (CM_Get_DevNode_Status(&status, &problem, info.DevInst, 0) == CR_SUCCESS) {
        driver.problem = static_cast<long>(problem);
        driver.disabled = problem == CM_PROB_DISABLED;
      }
      else {
        driver.problem = -1;
      }
      out.push_back(std::move(driver));
    }
    SetupDiDestroyDeviceInfoList(set);
    return true;
  }

  bool
  enumerate_endpoints(std::vector<endpoint_t> &out) {
    out.clear();
    // The request thread may already have COM in another apartment model: that is fine, only an unbalanced uninitialize is not.
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool initialized = SUCCEEDED(init);
    if (FAILED(init) && init != RPC_E_CHANGED_MODE) return false;

    bool ok = false;
    {
      com_ptr_t<IMMDeviceEnumerator> enumerator;
      if (SUCCEEDED(CoCreateInstance(clsid_enumerator, nullptr, CLSCTX_ALL, iid_enumerator, reinterpret_cast<void **>(enumerator.out()))) && enumerator) {
        read_flow(enumerator.p, eRender, out);
        read_flow(enumerator.p, eCapture, out);
        ok = true;
      }
    }
    if (initialized) CoUninitialize();
    return ok;
  }
}  // namespace audio_devices
