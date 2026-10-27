#pragma once

namespace spatial_audio {
  inline constexpr char mat_auto_pipe[] = "\\\\.\\pipe\\SunshineMatLive-active";
}

#if defined(_WIN32)
  #include "native_mat_guard.h"
  #include <shlobj.h>
  #pragma comment(lib, "advapi32.lib")
  #pragma comment(lib, "shell32.lib")
  #pragma comment(lib, "ole32.lib")
  #pragma comment(lib, "uuid.lib")

namespace spatial_audio {
  // Inputs are canonical paths. The tool deploys each executable below its SHA256.
  inline bool
  mat_auto_image(const std::filesystem::path &image, const std::filesystem::path &programs, const std::string &hash) {
    if (hash.size() != 64 || hash.find_first_not_of("0123456789abcdef") != std::string::npos) return false;
    const auto expected = programs / L"SunshineSonicMatLive";
    const auto directory = image.parent_path();
    return _wcsicmp(image.filename().c_str(), L"sunshine-plugin-mat-capture.exe") == 0 &&
           _wcsicmp(directory.parent_path().c_str(), expected.c_str()) == 0 &&
           directory.filename().wstring() == std::wstring(hash.begin(), hash.end());
  }
  inline bool
  trusted_mat_auto_publisher(HANDLE process) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation {};
    DWORD bytes = 0;
    const bool elevated = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &bytes) && elevation.TokenIsElevated;
    CloseHandle(token);
    if (!elevated) return false;
    PWSTR folder = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &folder))) return false;
    const std::filesystem::path programs(folder);
    CoTaskMemFree(folder);
    try {
      const auto image = std::filesystem::canonical(native_mat_guard::image(process));
      return mat_auto_image(image, std::filesystem::canonical(programs), native_mat_guard::sha256(image));
    }
    catch (...) {
      return false;
    }
  }
}  // namespace spatial_audio
#endif
