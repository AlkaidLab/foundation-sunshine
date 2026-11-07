#pragma once
// Exact system/vendor binaries whose internal offsets the capture tool and the
// MAT worker were verified against. These are the only place to add a build,
// and only after re-running native acceptance on it. No OS calls here: the
// worker inside audiodg includes this header.
#include <string_view>

namespace mat_compat {
  inline constexpr std::string_view audioeng_sha256[] {
    "3936bffdc4aa365e74c9995359f2821aaf759a14155a180cfa95764962b9f1d3",
  };

  struct DolbyBuild {
    std::string_view encoder_sha256, engine_sha256;
  };
  inline constexpr DolbyBuild dolby_builds[] {
    { "963697fb61ee302699b294b1b6cb59ab42e5eae3a555f6674ed54fc3a3cfacea", "342cbc58a19afa2b5b58cef408ef91dcc06c68829520286acabb7178eda7f569" },
  };

  inline constexpr wchar_t dolby_family[] = L"DolbyLaboratories.DolbyAccess_rz1tebttyb220";
  inline constexpr std::wstring_view dolby_package_prefix = L"DolbyLaboratories.DolbyAccess_";
  inline constexpr std::wstring_view dolby_package_suffix = L"_x64__rz1tebttyb220";
  inline constexpr std::wstring_view dolby_dll_directory = L"\\DLLs\\x64\\";

  inline bool
  supported_audioeng(std::string_view sha256) {
    for (auto pinned : audioeng_sha256)
      if (sha256 == pinned) return true;
    return false;
  }

  inline bool
  supported_dolby(std::string_view encoder_sha256, std::string_view engine_sha256) {
    for (const auto &build : dolby_builds)
      if (encoder_sha256 == build.encoder_sha256 && engine_sha256 == build.engine_sha256) return true;
    return false;
  }

  // <drive>:\...\WindowsApps\DolbyLaboratories.DolbyAccess_<version>_x64__rz1tebttyb220\DLLs\x64\
  // Store apps may live on any drive; the version segment changes with updates.
  inline bool
  dolby_directory_shape(std::wstring_view directory) {
    constexpr std::wstring_view apps = L"\\WindowsApps\\";
    if (directory.size() < 4 || directory.size() >= 260 || directory[1] != L':' || directory[2] != L'\\' ||
        directory.find(L"..") != std::wstring_view::npos || directory.find(L'/') != std::wstring_view::npos) {
      return false;
    }
    const auto at = directory.rfind(apps);
    if (at == std::wstring_view::npos) return false;
    auto package = directory.substr(at + apps.size());
    if (!package.starts_with(dolby_package_prefix) || !package.ends_with(dolby_dll_directory)) return false;
    package.remove_prefix(dolby_package_prefix.size());
    package.remove_suffix(dolby_dll_directory.size());
    if (!package.ends_with(dolby_package_suffix)) return false;
    package.remove_suffix(dolby_package_suffix.size());
    if (package.empty()) return false;
    for (auto c : package)
      if (c != L'.' && (c < L'0' || c > L'9')) return false;
    return true;
  }
}  // namespace mat_compat
