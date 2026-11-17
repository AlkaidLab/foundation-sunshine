#pragma once

#include <algorithm>

#ifndef NOMINMAX
  #define NOMINMAX
#endif
#include <windows.h>

#include <cwctype>
#include <string>
#include <string_view>

#include "src/version_compare.h"

#if __has_include("installer_version.h")
  #include "installer_version.h"
#endif

#ifndef SUNSHINE_INSTALLER_VERSION_W
  #define SUNSHINE_INSTALLER_VERSION_W L"unknown"
#endif

namespace installer_maintenance {

  inline std::wstring
  install_dir() {
    wchar_t buffer[1024] {};
    DWORD size = sizeof(buffer) - sizeof(wchar_t);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\AlkaidLab\\Sunshine", L"InstallDir",
          RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, buffer, &size) != ERROR_SUCCESS) return {};
    return buffer;
  }

  constexpr wchar_t kUninstallKey[] =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{B16E5C0D-7B8A-4E3B-9F1A-2D4C6E8F0A1B}_is1";

  struct installed_info {
    std::wstring directory, version, components, uninstall;
  };

  inline std::wstring
  registry_string(const wchar_t *key, const wchar_t *value) {
    wchar_t buffer[2048] {};
    for (const REGSAM view : { static_cast<REGSAM>(KEY_WOW64_64KEY), static_cast<REGSAM>(KEY_WOW64_32KEY) }) {
      DWORD size = sizeof(buffer) - sizeof(wchar_t);
      if (RegGetValueW(HKEY_LOCAL_MACHINE, key, value, RRF_RT_REG_SZ | view, nullptr, buffer, &size) == ERROR_SUCCESS)
        return buffer;
    }
    return {};
  }

  inline bool
  read_installed_info(installed_info *info) {
    // Authority: installer-inno-uninstall-regkey-20261107; installer-ms-uninstall-registry-20261107.
    // Inno's AppId key carries DisplayVersion, InstallLocation, the selected component list and the uninstaller.
    if (!info) return false;
    info->directory = install_dir();
    if (info->directory.empty()) info->directory = registry_string(kUninstallKey, L"InstallLocation");
    info->version = registry_string(kUninstallKey, L"DisplayVersion");
    info->components = registry_string(kUninstallKey, L"Inno Setup: Selected Components");
    info->uninstall = registry_string(kUninstallKey, L"UninstallString");
    return !info->directory.empty() &&
           (GetFileAttributesW((info->directory + L"\\sunshine.exe").c_str()) != INVALID_FILE_ATTRIBUTES ||
             !info->uninstall.empty());
  }

  inline std::wstring
  selected_components(const std::wstring &value) {
    std::wstring result;
    for (wchar_t ch : value)
      if ((ch >= L'a' && ch <= L'z') || ch == L',') result.push_back(ch);
    return result;
  }

  template <typename View>
  inline bool
  populate_view(View &view, std::wstring *uninstall) {
    installed_info existing;
    if (!read_installed_info(&existing)) return false;
    view.maintenance = true;
    view.page = decltype(view.page)::maintenance;
    view.installed_version = existing.version.empty() ? L"未知" : existing.version;
    view.package_version = SUNSHINE_INSTALLER_VERSION_W;
    // Compare only two real versions: the first three parts (YYYY.MDD.HMMSS); .杂鱼, a commit and any other suffix do not count, a manual-... build
    // goes by its embedded date. A side with no date version is unknown, never a downgrade or the same version.
    const auto relation = version_compare::compare(std::wstring_view(view.package_version), std::wstring_view(existing.version));
    view.maintenance_downgrade = relation && *relation < 0;
    view.maintenance_same = relation && *relation == 0;
    if (uninstall) *uninstall = existing.uninstall;
    view.has_uninstaller = !existing.uninstall.empty();
    const std::wstring selected = selected_components(existing.components);
    if (!selected.empty()) {
      static const wchar_t *names[] = { L"vdd", L"gamepad", L"usbip", L"vmouse", L"firewall", L"autostart", L"tools" };
      for (int i = 0; i < 7; ++i) view.tiles[i] = selected.find(names[i]) != std::wstring::npos;
    }
    view.tiles[7] = true;
    return true;
  }

  inline bool
  split_uninstall_command(const std::wstring &command, std::wstring *file, std::wstring *args) {
    if (!file || !args) return false;
    size_t begin = 0;
    while (begin < command.size() && std::iswspace(command[begin])) ++begin;
    if (begin == command.size()) return false;
    size_t end = begin;
    if (command[begin] == L'"') {
      end = command.find(L'"', ++begin);
      if (end == std::wstring::npos) return false;
      *file = command.substr(begin, end - begin);
      ++end;
    }
    else {
      end = command.find_first_of(L" \t", begin);
      *file = command.substr(begin, end == std::wstring::npos ? std::wstring::npos : end - begin);
    }
    while (end < command.size() && std::iswspace(command[end])) ++end;
    *args = command.substr(end);
    return !file->empty();
  }

  inline bool
  start_uninstaller(const std::wstring &command) {
    // Authority: installer-ms-shellexecuteex-20261107; preserve the registered command and elevate it with runas.
    std::wstring file, args;
    if (!split_uninstall_command(command, &file, &args)) return false;
    SHELLEXECUTEINFOW info {};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas";
    info.lpFile = file.c_str();
    info.lpParameters = args.empty() ? nullptr : args.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) return false;
    if (info.hProcess) CloseHandle(info.hProcess);
    return true;
  }

}  // namespace installer_maintenance
