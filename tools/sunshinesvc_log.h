/**
 * @file tools/sunshinesvc_log.h
 * @brief Service log sink selection for the Windows service wrapper.
 *
 * The log handle is inheritable and handed to Sunshine.exe as its std
 * handles, so it outlives the service whenever the core lingers. Sink
 * selection must therefore never abort service startup (#1119): a primary
 * log that is held by another writer (or unusable) degrades to a
 * per-instance file, then to NUL.
 */
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <string>

namespace sunshinesvc {

  // An array (not a pointer): sizeof must see the full literal so the
  // suffix reservation below stays tied to the actual name.
  constexpr wchar_t LOG_SUFFIX[] = L"sunshine.log";

  // Paths at or beyond MAX_PATH cannot be opened by CreateFileW without a
  // \\?\ prefix. Failing the primary path fast keeps the fallback chain in
  // charge of recovery; the reservation covers the suffix and its NUL to
  // mirror a fixed MAX_PATH destination buffer.
  inline bool
  primary_log_path_fits(size_t temp_path_length, size_t buffer_capacity) {
    constexpr auto suffix_length = sizeof(LOG_SUFFIX) / sizeof(wchar_t) - 1;
    return temp_path_length > 0 && temp_path_length + suffix_length < buffer_capacity;
  }

  // The per-instance fallback builds its name on the heap, so only the temp
  // path itself has to fit the probe buffer.
  inline bool
  fallback_temp_path_usable(size_t temp_path_length, size_t buffer_capacity) {
    return temp_path_length > 0 && temp_path_length < buffer_capacity;
  }

  inline std::wstring
  fallback_log_name(const std::wstring &temp_path, DWORD process_id) {
    return temp_path + L"sunshine-" + std::to_wstring(process_id) + L".log";
  }

  // Opens a fresh (truncated) log. The share mode deliberately refuses
  // co-writers: while any process still holds a write handle on the file —
  // a lingering core from any build — this open fails with
  // ERROR_SHARING_VIOLATION and the caller falls back, instead of
  // CREATE_ALWAYS truncating a log that someone else still owns.
  inline HANDLE
  open_log_file(const wchar_t *file_name) {
    SECURITY_ATTRIBUTES security_attributes = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
    return CreateFileW(file_name,
      GENERIC_WRITE,
      FILE_SHARE_READ,
      &security_attributes,
      CREATE_ALWAYS,
      0,
      NULL);
  }

  inline HANDLE
  open_nul_device() {
    SECURITY_ATTRIBUTES security_attributes = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
    return CreateFileW(L"NUL",
      GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE,
      &security_attributes,
      OPEN_EXISTING,
      0,
      NULL);
  }

  inline std::wstring
  with_trailing_separator(const std::wstring &directory) {
    if (directory.empty() || (directory.back() != L'\\' && directory.back() != L'/')) {
      return directory + L'\\';
    }
    return directory;
  }

  // Directory-injectable core so unit tests run against isolated temporary
  // directories instead of the shared sunshine.log location.
  inline HANDLE
  open_primary_log_handle_in(const std::wstring &temp_dir) {
    const auto directory = with_trailing_separator(temp_dir);
    if (!primary_log_path_fits(directory.size(), MAX_PATH)) {
      return INVALID_HANDLE_VALUE;
    }
    return open_log_file((directory + LOG_SUFFIX).c_str());
  }

  inline HANDLE
  open_fallback_log_handle_in(const std::wstring &temp_dir) {
    const auto directory = with_trailing_separator(temp_dir);
    if (fallback_temp_path_usable(directory.size(), MAX_PATH)) {
      const auto handle = open_log_file(fallback_log_name(directory, GetCurrentProcessId()).c_str());
      if (handle != INVALID_HANDLE_VALUE) {
        return handle;
      }
    }

    // The temp path is unusable or the per-instance name failed: NUL keeps
    // the service startable even without a log sink.
    return open_nul_device();
  }

  inline HANDLE
  open_primary_log_handle() {
    wchar_t temp_path[MAX_PATH];
    const auto temp_path_length = GetTempPathW(_countof(temp_path), temp_path);
    if (temp_path_length == 0 || temp_path_length >= _countof(temp_path)) {
      return INVALID_HANDLE_VALUE;
    }
    return open_primary_log_handle_in(std::wstring(temp_path, temp_path_length));
  }

  // A core process left over from any build can hold the primary name, so
  // fall back to a per-instance file before giving up on logging entirely.
  inline HANDLE
  open_fallback_log_handle() {
    wchar_t temp_path[MAX_PATH];
    const auto temp_path_length = GetTempPathW(_countof(temp_path), temp_path);
    if (temp_path_length == 0 || temp_path_length >= _countof(temp_path)) {
      return open_nul_device();
    }
    return open_fallback_log_handle_in(std::wstring(temp_path, temp_path_length));
  }
}  // namespace sunshinesvc
