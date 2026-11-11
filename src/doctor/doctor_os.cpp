/**
 * @file src/doctor/doctor_os.cpp
 * @brief Read-only operating system facts for the health check; one half per platform.
 */
#include "doctor_os.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <system_error>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <mmdeviceapi.h>
  #include <propidl.h>
  #include <setupapi.h>
  #include <winsvc.h>
#else
  #include <cerrno>
  #include <cstring>
  #include <unistd.h>
  #ifdef __linux__
    #include <sys/timex.h>
  #endif
#endif

namespace fs = std::filesystem;

namespace doctor::os {
  namespace {
    std::string
    lower(std::string text) {
      std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return text;
    }

    /** Age in seconds of the newest file in a folder whose name starts with a prefix; one entry per match. */
    std::vector<std::int64_t>
    ages_of(const fs::path &folder, const std::vector<std::string> &prefixes, std::int64_t window_s, bool *readable) {
      std::vector<std::int64_t> ages;
      std::error_code ec;
      if (!fs::is_directory(folder, ec)) {
        return ages;
      }
      *readable = true;
      const auto now = fs::file_time_type::clock::now();
      for (fs::directory_iterator it { folder, ec }, end; !ec && it != end; it.increment(ec)) {
        const auto name = lower(it->path().filename().string());
        if (std::none_of(prefixes.begin(), prefixes.end(), [&](const auto &p) { return name.rfind(p, 0) == 0; })) continue;
        std::error_code time_ec;
        const auto written = fs::last_write_time(it->path(), time_ec);
        if (time_ec) continue;
        const auto age = std::chrono::duration_cast<std::chrono::seconds>(now - written).count();
        if (age >= 0 && age <= window_s) ages.push_back(age);
      }
      return ages;
    }

#ifdef _WIN32
    std::string
    utf8(const wchar_t *text) {
      if (!text || !*text) return {};
      const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
      if (size <= 1) return {};
      std::string out(static_cast<std::size_t>(size - 1), '\0');
      WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
      return out;
    }

    std::optional<std::wstring>
    registry_string(HKEY root, const wchar_t *path, const wchar_t *name) {
      DWORD size = 0;
      if (RegGetValueW(root, path, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS || size == 0) {
        return std::nullopt;
      }
      std::wstring value(size / sizeof(wchar_t), L'\0');
      if (RegGetValueW(root, path, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, value.data(), &size) != ERROR_SUCCESS) {
        return std::nullopt;
      }
      value.resize(wcsnlen(value.c_str(), value.size()));
      return value;
    }

    bool
    registry_key_exists(HKEY root, const wchar_t *path) {
      HKEY key = nullptr;
      if (RegOpenKeyExW(root, path, 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
      RegCloseKey(key);
      return true;
    }

    bool
    registry_value_present(HKEY root, const wchar_t *path, const wchar_t *name) {
      DWORD size = 0;
      return RegGetValueW(root, path, name, RRF_RT_ANY, nullptr, nullptr, &size) == ERROR_SUCCESS && size > sizeof(wchar_t) * 2;
    }

    /** Service state, or 0 when the service does not exist or cannot be queried. */
    DWORD
    service_state(const wchar_t *name) {
      SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
      if (!manager) return 0;
      DWORD state = 0;
      if (SC_HANDLE service = OpenServiceW(manager, name, SERVICE_QUERY_STATUS)) {
        SERVICE_STATUS status {};
        if (QueryServiceStatus(service, &status)) state = status.dwCurrentState;
        CloseServiceHandle(service);
      }
      CloseServiceHandle(manager);
      return state;
    }

    // Property keys of an audio endpoint (functiondiscoverykeys_devpkey.h), spelled out so the file needs no extra header.
    constexpr PROPERTYKEY doctor_key_friendly_name { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };
    constexpr PROPERTYKEY doctor_key_device_desc { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 2 };
    constexpr PROPERTYKEY doctor_key_interface_name { { 0x026e516e, 0xb814, 0x414b, { 0x83, 0xcd, 0x85, 0x6d, 0x6f, 0xef, 0x48, 0x22 } }, 2 };

    std::string
    endpoint_property(IPropertyStore *store, const PROPERTYKEY &key) {
      PROPVARIANT value;
      PropVariantInit(&value);
      std::string out;
      if (SUCCEEDED(store->GetValue(key, &value)) && value.vt == VT_LPWSTR) out = utf8(value.pwszVal);
      PropVariantClear(&value);
      return out;
    }

    template <class T>
    struct com_ptr_t {
      T *p = nullptr;
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
    };
#endif
  }  // namespace

  bool
  endpoint_matches(const audio_endpoint_t &endpoint, const std::string &configured) {
    if (configured.empty()) return false;
    const auto wanted = lower(configured);
    for (const auto *field : { &endpoint.id, &endpoint.name, &endpoint.description, &endpoint.adapter }) {
      if (!field->empty() && lower(*field) == wanted) return true;
    }
    return false;
  }

#ifdef _WIN32

  time_sync_t
  time_sync() {
    time_sync_t result;
    const auto type = registry_string(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\W32Time\\Parameters", L"Type");
    if (!type) return result;
    result.checked = true;
    const auto mode = utf8(type->c_str());
    // W32Time is trigger-started: a stopped service is normal and starts for each sync. NoSync turns syncing off.
    result.synced = lower(mode) != "nosync";
    const auto server = registry_string(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\W32Time\\Parameters", L"NtpServer");
    result.source = mode == "NT5DS" ? "Domain (NT5DS)" : mode + (server ? " " + utf8(server->c_str()) : "");
    const auto state = service_state(L"W32Time");
    result.detail = state == SERVICE_RUNNING ? "W32Time running" : state == 0 ? "W32Time missing" : "W32Time stopped (trigger start)";
    if (state == 0) result.synced = false;
    return result;
  }

  pending_reboot_t
  pending_reboot() {
    pending_reboot_t result;
    result.checked = true;
    if (registry_key_exists(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Component Based Servicing\\RebootPending")) result.reasons.emplace_back("cbs");
    if (registry_key_exists(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\WindowsUpdate\\Auto Update\\RebootRequired")) result.reasons.emplace_back("wu");
    if (registry_value_present(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager", L"PendingFileRenameOperations")) result.reasons.emplace_back("file_rename");
    return result;
  }

  crashes_t
  recent_crashes(std::int64_t window_s) {
    crashes_t result;
    result.source = "WER";
    wchar_t buffer[MAX_PATH] {};
    if (!GetEnvironmentVariableW(L"ProgramData", buffer, MAX_PATH)) return result;
    const fs::path wer = fs::path { buffer } / "Microsoft" / "Windows" / "WER";
    bool readable = false;
    // Windows Error Reporting keeps one folder per crash: AppCrash_<exe>_<hash>_<id>.
    for (const auto *folder : { "ReportArchive", "ReportQueue" }) {
      auto ages = ages_of(wer / folder, { "appcrash_sunshine.exe", "appcrash_sunshine-gui.exe" }, window_s, &readable);
      result.ages_s.insert(result.ages_s.end(), ages.begin(), ages.end());
    }
    result.checked = readable;
    std::sort(result.ages_s.begin(), result.ages_s.end());
    return result;
  }

  std::string
  account_kind() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
    std::string kind = "user";
    BYTE buffer[SECURITY_MAX_SID_SIZE];
    DWORD size = sizeof(buffer);
    BOOL member = FALSE;
    if (CreateWellKnownSid(WinLocalSystemSid, nullptr, buffer, &size)) {
      // The process token of a service: compare the user SID directly.
      BYTE user_buffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
      DWORD user_size = 0;
      if (GetTokenInformation(token, TokenUser, user_buffer, sizeof(user_buffer), &user_size) &&
          EqualSid(reinterpret_cast<TOKEN_USER *>(user_buffer)->User.Sid, buffer)) {
        kind = "system";
      }
    }
    if (kind == "user") {
      size = sizeof(buffer);
      if (CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, buffer, &size) && CheckTokenMembership(nullptr, buffer, &member) && member) {
        kind = "admin";
      }
    }
    CloseHandle(token);
    return kind;
  }

  access_t
  writable(const std::string &path) {
    access_t result;
    std::error_code ec;
    result.checked = fs::exists(fs::u8path(path), ec);
    // Windows ACLs decide; the folder's read-only bit is the part a plain status can see.
    result.ok = result.checked && (fs::status(fs::u8path(path), ec).permissions() & fs::perms::owner_write) != fs::perms::none;
    return result;
  }

  audio_endpoints_t
  audio_endpoints() {
    audio_endpoints_t result;
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool owns_com = SUCCEEDED(init);
    {
      com_ptr_t<IMMDeviceEnumerator> enumerator;
      if (FAILED(CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, IID_IMMDeviceEnumerator, reinterpret_cast<void **>(enumerator.out())))) {
        result.error = "MMDeviceEnumerator";
      }
      else {
        result.checked = true;
        std::string default_id;
        {
          com_ptr_t<IMMDevice> device;
          if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.out()))) {
            LPWSTR id = nullptr;
            if (SUCCEEDED(device->GetId(&id))) {
              default_id = utf8(id);
              CoTaskMemFree(id);
            }
          }
        }
        com_ptr_t<IMMDeviceCollection> collection;
        UINT count = 0;
        if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, collection.out())) && SUCCEEDED(collection->GetCount(&count))) {
          for (UINT i = 0; i < count; ++i) {
            com_ptr_t<IMMDevice> device;
            if (FAILED(collection->Item(i, device.out()))) continue;
            audio_endpoint_t endpoint;
            LPWSTR id = nullptr;
            if (SUCCEEDED(device->GetId(&id))) {
              endpoint.id = utf8(id);
              CoTaskMemFree(id);
            }
            com_ptr_t<IPropertyStore> store;
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, store.out()))) {
              endpoint.name = endpoint_property(store.p, doctor_key_friendly_name);
              endpoint.description = endpoint_property(store.p, doctor_key_device_desc);
              endpoint.adapter = endpoint_property(store.p, doctor_key_interface_name);
            }
            endpoint.is_default = !default_id.empty() && endpoint.id == default_id;
            result.render.push_back(std::move(endpoint));
          }
        }
      }
    }
    if (owns_com) CoUninitialize();
    return result;
  }

  usbip_t
  usbip() {
    usbip_t result;
    result.supported = true;
    wchar_t found[MAX_PATH] {};
    if (SearchPathW(nullptr, L"usbip.exe", nullptr, MAX_PATH, found, nullptr)) {
      result.tool_found = true;
      result.tool_path = utf8(found);
    }
    else {
      // The usbip-win2 installer does not add itself to PATH; the host looks in Program Files too.
      for (const auto *variable : { L"ProgramW6432", L"ProgramFiles" }) {
        wchar_t base[MAX_PATH] {};
        if (!GetEnvironmentVariableW(variable, base, MAX_PATH)) continue;
        const auto candidate = fs::path { base } / "USBip" / "usbip.exe";
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec)) {
          result.tool_found = true;
          const auto text = candidate.u8string();
          result.tool_path.assign(reinterpret_cast<const char *>(text.c_str()), text.size());
          break;
        }
      }
    }
    result.driver_checked = true;
    for (const auto *service : { L"usbip2_ude", L"usbip2_vhci" }) {
      std::wstring path = L"SYSTEM\\CurrentControlSet\\Services\\";
      path += service;
      if (registry_key_exists(HKEY_LOCAL_MACHINE, path.c_str())) {
        result.driver_found = true;
        result.driver = utf8(service);
        break;
      }
    }
    return result;
  }

  std::string
  vdd_driver_version() {
    // GUID_DEVCLASS_DISPLAY (devguid.h)
    static const GUID display_class { 0x4d36e968, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 } };
    HDEVINFO devices = SetupDiGetClassDevsW(&display_class, nullptr, nullptr, DIGCF_PRESENT);
    if (devices == INVALID_HANDLE_VALUE) return {};
    std::string version;
    SP_DEVINFO_DATA info {};
    info.cbSize = sizeof(info);
    for (DWORD index = 0; version.empty() && SetupDiEnumDeviceInfo(devices, index, &info); ++index) {
      wchar_t ids[1024] {};
      if (!SetupDiGetDeviceRegistryPropertyW(devices, &info, SPDRP_HARDWAREID, nullptr, reinterpret_cast<PBYTE>(ids), sizeof(ids) - sizeof(wchar_t), nullptr)) continue;
      bool match = false;
      for (const wchar_t *id = ids; *id; id += wcslen(id) + 1) {
        if (lstrcmpiW(id, L"ROOT\\ZAKOVDD") == 0) match = true;
      }
      if (!match) continue;
      // SPDRP_DRIVER names the driver key under Control\Class, which holds DriverVersion.
      wchar_t driver[256] {};
      if (!SetupDiGetDeviceRegistryPropertyW(devices, &info, SPDRP_DRIVER, nullptr, reinterpret_cast<PBYTE>(driver), sizeof(driver) - sizeof(wchar_t), nullptr)) continue;
      std::wstring key = L"SYSTEM\\CurrentControlSet\\Control\\Class\\";
      key += driver;
      if (const auto value = registry_string(HKEY_LOCAL_MACHINE, key.c_str(), L"DriverVersion")) version = utf8(value->c_str());
    }
    SetupDiDestroyDeviceInfoList(devices);
    return version;
  }

  std::optional<int>
  windows_build() {
    using rtl_get_version_fn = LONG(WINAPI *)(OSVERSIONINFOEXW *);
    const auto ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto rtl_get_version = ntdll ? reinterpret_cast<rtl_get_version_fn>(reinterpret_cast<void *>(GetProcAddress(ntdll, "RtlGetVersion"))) : nullptr;
    if (!rtl_get_version) return std::nullopt;
    OSVERSIONINFOEXW version {};
    version.dwOSVersionInfoSize = sizeof(version);
    if (rtl_get_version(&version) < 0) return std::nullopt;
    return static_cast<int>(version.dwBuildNumber);
  }

#else

  time_sync_t
  time_sync() {
    time_sync_t result;
  #ifdef __linux__
    // adjtimex with modes 0 only reads the kernel clock discipline (adjtimex(2)).
    struct timex tx {};
    const int state = adjtimex(&tx);
    if (state < 0) return result;
    result.checked = true;
    result.synced = state != TIME_ERROR && !(tx.status & STA_UNSYNC);
    result.source = "kernel NTP";
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "error ±%ld ms", static_cast<long>(tx.esterror / 1000));
    result.detail = result.synced ? buffer : "";
  #endif
    return result;
  }

  pending_reboot_t
  pending_reboot() {
    pending_reboot_t result;
    std::error_code ec;
    // Debian and Ubuntu write this file when an update needs a restart; other distributions keep no such record.
    if (fs::exists("/etc/debian_version", ec)) {
      result.checked = true;
      if (fs::exists("/var/run/reboot-required", ec)) result.reasons.emplace_back("packages");
    }
    return result;
  }

  crashes_t
  recent_crashes(std::int64_t window_s) {
    crashes_t result;
    result.source = "systemd-coredump";
    bool readable = false;
    result.ages_s = ages_of("/var/lib/systemd/coredump", { "core.sunshine." }, window_s, &readable);
    result.checked = readable;
    std::sort(result.ages_s.begin(), result.ages_s.end());
    return result;
  }

  std::string
  account_kind() {
    return geteuid() == 0 ? "root" : "user";
  }

  access_t
  writable(const std::string &path) {
    access_t result;
    result.checked = true;
    if (access(path.c_str(), W_OK) == 0) {
      result.ok = true;
    }
    else {
      result.error = std::strerror(errno);
    }
    return result;
  }

  audio_endpoints_t
  audio_endpoints() {
    return {};
  }

  usbip_t
  usbip() {
    return {};
  }

  std::string
  vdd_driver_version() {
    return {};
  }

  std::optional<int>
  windows_build() {
    return std::nullopt;
  }

#endif
}  // namespace doctor::os
