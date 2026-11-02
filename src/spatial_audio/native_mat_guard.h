#pragma once
#ifndef NOMINMAX
  #define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <string>
#include <vector>
#include <filesystem>
#include "mat_compat.h"
#pragma comment(lib, "bcrypt.lib")
namespace native_mat_guard {
  inline std::wstring
  system(const wchar_t *file) {
    wchar_t dir[MAX_PATH];
    DWORD n = GetSystemDirectoryW(dir, MAX_PATH);
    return n && n < MAX_PATH ? (std::filesystem::path(dir) / file).wstring() : L"";
  }
  inline std::wstring
  image(HANDLE process) {
    wchar_t path[32768];
    DWORD n = 32768;
    return QueryFullProcessImageNameW(process, 0, path, &n) ? std::wstring(path, n) : L"";
  }
  inline uint64_t
  created(HANDLE process) {
    FILETIME c {}, e {}, k {}, u {};
    return GetProcessTimes(process, &c, &e, &k, &u) ? (uint64_t(c.dwHighDateTime) << 32) | c.dwLowDateTime : 0;
  }
  inline std::string
  sha256(const std::filesystem::path &path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    BCRYPT_ALG_HANDLE algorithm {};
    BCRYPT_HASH_HANDLE hash {};
    DWORD object_size = 0, got = 0;
    std::string result;
    std::vector<unsigned char> object;
    unsigned char digest[32], buffer[65536];
    bool okay = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
                BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size), 4, &got, 0) >= 0;
    if (okay) {
      object.resize(object_size);
      okay = BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0) >= 0;
    }
    while (okay) {
      if (!ReadFile(file, buffer, sizeof(buffer), &got, nullptr)) {
        okay = false;
        break;
      }
      if (!got) break;
      okay = BCryptHashData(hash, buffer, got, 0) >= 0;
    }
    if (okay && BCryptFinishHash(hash, digest, 32, 0) >= 0) {
      const char *hex = "0123456789abcdef";
      for (auto b : digest) {
        result += hex[b >> 4];
        result += hex[b & 15];
      }
    }
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);
    return result;
  }
  inline bool
  system_engine(HANDLE process) {
    return _wcsicmp(image(process).c_str(), system(L"audiodg.exe").c_str()) == 0 &&
           mat_compat::supported_audioeng(sha256(system(L"audioeng.dll")));
  }
  inline bool
  loaded_audioeng(HMODULE module) {
    wchar_t path[32768];
    DWORD n = GetModuleFileNameW(module, path, 32768);
    return module && n && n < 32768 && _wcsicmp(path, system(L"audioeng.dll").c_str()) == 0;
  }
}  // namespace native_mat_guard
