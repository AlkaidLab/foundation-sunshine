#include "src/sparxie/install.h"

#include <fstream>
#include <initializer_list>
#include <string_view>

#if defined(_WIN32)
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#endif

namespace sparxie {
#if defined(_WIN32)
  namespace {
    const char kSep = static_cast<char>(92);

    std::wstring
    wide(std::string_view text) {
      if (text.empty()) return {};
      const int chars = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
      std::wstring out(chars > 0 ? static_cast<std::size_t>(chars) : 0, L'\0');
      if (chars > 0) MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), chars);
      return out;
    }

    std::string
    narrow(std::wstring_view text) {
      if (text.empty()) return {};
      const int chars = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
      std::string out(chars > 0 ? static_cast<std::size_t>(chars) : 0, '\0');
      if (chars > 0) WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), chars, nullptr, nullptr);
      return out;
    }

    std::string
    join_key(std::initializer_list<const char *> parts) {
      std::string out;
      for (const char *part : parts) {
        if (!out.empty()) out.push_back(kSep);
        out += part;
      }
      return out;
    }

    std::string
    reg_sz(HKEY root, const std::string &sub, const wchar_t *name) {
      const std::wstring key = wide(sub);
      DWORD bytes = 0;
      if (RegGetValueW(root, key.c_str(), name, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS || bytes < sizeof(wchar_t)) return {};
      std::wstring value(bytes / sizeof(wchar_t), L'\0');
      if (RegGetValueW(root, key.c_str(), name, RRF_RT_REG_SZ, nullptr, value.data(), &bytes) != ERROR_SUCCESS) return {};
      while (!value.empty() && value.back() == L'\0') value.pop_back();
      return narrow(value);
    }

    bool
    usable_version(const std::string &version) {
      return !version.empty() && version != "0.0.0.0";
    }

    bool
    can_write(const fs::path &dest) {
      std::error_code ec;
      fs::path dir = dest;
      fs::create_directories(dir, ec);
      while (!dir.empty() && !fs::is_directory(dir, ec)) dir = dir.parent_path();
      if (dir.empty()) return false;
      const fs::path probe = dir / "sparxie-write-probe";
      std::ofstream out(probe);
      if (!out) return false;
      out << "ok";
      out.close();
      fs::remove(probe, ec);
      return true;
    }
  }  // namespace

  // Windows build: RtlGetVersion, not GetVersionEx (ms-rtlgetversion-20261110).
  // Free space: GetDiskFreeSpaceExW (ms-getdiskfreespaceexw-20261110).
  // WebView2: pv under the Evergreen client id (ms-webview2-pv-20261110).
  // VC++ 14 x64: Installed under VC\Runtimes\x64 (ms-vc-redist-14-runtimes-20261110).
  // Elevation: TOKEN_ELEVATION and TOKEN_ELEVATION_TYPE (ms-token-elevation-20261110, ms-token-elevation-type-20261110).
  // A missing file after extract is reported as quarantined. No exclusion is added (ms-defender-quarantine-20261110).
  Probes
  current_probes(const fs::path &dest) {
    Probes probes;
    probes.supplied = true;
    using RtlGetVersionPtr = LONG(WINAPI *)(OSVERSIONINFOW *);
    if (const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
      auto rtl = reinterpret_cast<RtlGetVersionPtr>(GetProcAddress(ntdll, "RtlGetVersion"));
      OSVERSIONINFOEXW info {};
      info.dwOSVersionInfoSize = sizeof(info);
      if (rtl && rtl(reinterpret_cast<OSVERSIONINFOW *>(&info)) == 0) {
        probes.os_known = true;
        probes.os_build = static_cast<int>(info.dwBuildNumber);
      }
    }
    SYSTEM_INFO info {};
    GetNativeSystemInfo(&info);
    probes.os_x64 = info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64;

    fs::path volume = dest;
    std::error_code ec;
    fs::create_directories(volume, ec);
    while (!volume.empty() && !fs::exists(volume, ec)) volume = volume.parent_path();
    ULARGE_INTEGER avail {};
    if (!volume.empty() && GetDiskFreeSpaceExW(volume.c_str(), &avail, nullptr, nullptr)) {
      probes.free_known = true;
      probes.free_bytes = avail.QuadPart;
    }

    const std::string client = "{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}";
    const std::string machine = join_key({ "SOFTWARE", "WOW6432Node", "Microsoft", "EdgeUpdate", "Clients", client.c_str() });
    const std::string user = join_key({ "Software", "Microsoft", "EdgeUpdate", "Clients", client.c_str() });
    std::string web = reg_sz(HKEY_LOCAL_MACHINE, machine, L"pv");
    if (!usable_version(web)) web = reg_sz(HKEY_CURRENT_USER, user, L"pv");
    probes.webview2_known = true;
    probes.webview2 = usable_version(web);
    probes.webview2_version = web;

    const std::string runtime = join_key({ "SOFTWARE", "Microsoft", "VisualStudio", "14.0", "VC", "Runtimes", "x64" });
    const std::wstring runtime_key = wide(runtime);
    DWORD installed = 0;
    DWORD bytes = sizeof(installed);
    const bool have_dword = RegGetValueW(HKEY_LOCAL_MACHINE, runtime_key.c_str(), L"Installed", RRF_RT_REG_DWORD, nullptr, &installed, &bytes) == ERROR_SUCCESS;
    const std::string version = reg_sz(HKEY_LOCAL_MACHINE, runtime, L"Version");
    probes.vcruntime_known = true;
    probes.vcruntime = have_dword && installed == 1 && !version.empty();
    probes.vcruntime_version = version;

    probes.write_known = true;
    probes.can_write = can_write(dest.empty() ? fs::path(".") : dest);
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
      DWORD got = 0;
      TOKEN_ELEVATION elevation {};
      const bool elevated = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &got) && elevation.TokenIsElevated;
      TOKEN_ELEVATION_TYPE type = TokenElevationTypeDefault;
      if (GetTokenInformation(token, TokenElevationType, &type, sizeof(type), &got)) {
        probes.elevation_known = true;
        probes.can_elevate = elevated || type == TokenElevationTypeFull || type == TokenElevationTypeLimited;
      }
      CloseHandle(token);
    }
    return probes;
  }
#else
  Probes
  current_probes(const fs::path &) {
    return {};
  }
#endif
}  // namespace sparxie
