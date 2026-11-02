#pragma once
// Read-only description of the system components live MAT capture depends on,
// printed by Preflight/Start. It never decides trust: the hooks and the worker
// re-check the pinned hashes in src/spatial_audio/mat_compat.h themselves.
//
// Dolby Access is resolved through its package family instead of a
// version-specific WindowsApps path. This must run in the interactive user's
// context (elevated is fine): package registrations are per user, so audiodg
// cannot do it and receives the verified directory from the publisher.
#include "../../src/spatial_audio/native_mat_guard.h"
#include <appmodel.h>
#include <winver.h>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>
#pragma comment(lib, "version.lib")

namespace mat_compat {
  enum class Support { supported,
    missing,
    unsupported };

  inline const char *
  support_name(Support support) {
    switch (support) {
      case Support::supported:
        return "supported";
      case Support::missing:
        return "missing";
      default:
        return "unsupported";
    }
  }

  struct Component {
    std::string name;
    Support support = Support::missing;
    std::string version = "unknown";
    std::string sha256;
  };

  inline std::string
  file_version(const std::wstring &path) {
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (!size) return "unknown";
    std::vector<BYTE> data(size);
    VS_FIXEDFILEINFO *info = nullptr;
    UINT length = 0;
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()) ||
        !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void **>(&info), &length) || !info || length < sizeof(*info)) {
      return "unknown";
    }
    return std::to_string(HIWORD(info->dwFileVersionMS)) + '.' + std::to_string(LOWORD(info->dwFileVersionMS)) + '.' +
           std::to_string(HIWORD(info->dwFileVersionLS)) + '.' + std::to_string(LOWORD(info->dwFileVersionLS));
  }

  inline Component
  audio_engine() {
    Component component { "audioeng.dll" };
    const auto path = native_mat_guard::system(L"audioeng.dll");
    component.sha256 = native_mat_guard::sha256(path);
    if (component.sha256.empty()) return component;
    component.version = file_version(path);
    component.support = supported_audioeng(component.sha256) ? Support::supported : Support::unsupported;
    return component;
  }
}  // namespace mat_compat

namespace sonic_live {
  struct DolbyAccess {
    mat_compat::Component component { "DolbyAccess" };
    std::wstring directory;
  };

  inline DolbyAccess
  find_dolby_access() {
    DolbyAccess result;
    UINT32 count = 0, length = 0;
    if (GetPackagesByPackageFamily(mat_compat::dolby_family, &count, nullptr, &length, nullptr) != ERROR_INSUFFICIENT_BUFFER || !count) return result;
    std::vector<PWSTR> names(count);
    std::vector<wchar_t> buffer(length);
    if (GetPackagesByPackageFamily(mat_compat::dolby_family, &count, names.data(), &length, buffer.data()) != ERROR_SUCCESS) return result;
    for (UINT32 i = 0; i < count; ++i) {
      const std::wstring_view full = names[i];
      if (!full.starts_with(mat_compat::dolby_package_prefix) || !full.ends_with(mat_compat::dolby_package_suffix)) continue;
      UINT32 path_length = 0;
      if (GetPackagePathByFullName(names[i], &path_length, nullptr) != ERROR_INSUFFICIENT_BUFFER || !path_length) continue;
      std::wstring root(path_length, L'\0');
      if (GetPackagePathByFullName(names[i], &path_length, root.data()) != ERROR_SUCCESS) continue;
      root.resize(wcslen(root.c_str()));
      auto version = full.substr(mat_compat::dolby_package_prefix.size());
      version.remove_suffix(mat_compat::dolby_package_suffix.size());
      result.component.version.clear();
      for (auto c : version) result.component.version.push_back(static_cast<char>(c));  // digits and dots only
      result.directory = root + std::wstring(mat_compat::dolby_dll_directory);
      const auto encoder = native_mat_guard::sha256(result.directory + L"DolbyMatEnc.dll");
      const auto engine = native_mat_guard::sha256(result.directory + L"DolbyMatEngine.dll");
      result.component.sha256 = encoder + "," + engine;
      const bool supported = !encoder.empty() && !engine.empty() && mat_compat::supported_dolby(encoder, engine) &&
                             mat_compat::dolby_directory_shape(result.directory);
      result.component.support = supported ? mat_compat::Support::supported : mat_compat::Support::unsupported;
      return result;
    }
    return result;
  }

  // One line per component so operators can paste them into a report.
  inline bool
  report_components(const mat_compat::Component &engine, const mat_compat::Component &dolby) {
    for (const auto *component : { &engine, &dolby }) {
      printf("COMPONENT name=%s support=%s version=%s sha256=%s\n", component->name.c_str(), mat_compat::support_name(component->support),
        component->version.c_str(), component->sha256.empty() ? "none" : component->sha256.c_str());
    }
    return engine.support == mat_compat::Support::supported && dolby.support == mat_compat::Support::supported;
  }
}  // namespace sonic_live
