/**
 * @file tools/sunshinesvc_log.h
 * @brief Service log sink selection for the Windows service wrapper.
 *
 * The log handle is inheritable and handed to Sunshine.exe as its std
 * handles, so it outlives the service whenever the core lingers. Sink
 * selection must therefore never abort service startup (#1119): a locked
 * or unusable primary log degrades to a per-instance file, then to NUL.
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

  // wcscat_s runs the invalid parameter handler (process termination by
  // default) when the destination buffer is too small, which would defeat
  // the fallback chain, so the fixed-buffer primary path must reserve room
  // for the suffix and its terminating NUL.
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

  inline HANDLE
  open_log_handle(const wchar_t *file_name) {
    SECURITY_ATTRIBUTES security_attributes = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
    return CreateFileW(file_name,
      GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE,
      &security_attributes,
      CREATE_ALWAYS,
      0,
      NULL);
  }

  inline HANDLE
  open_primary_log_handle() {
    wchar_t log_file_name[MAX_PATH];
    const auto temp_path_length = GetTempPathW(_countof(log_file_name), log_file_name);
    if (!primary_log_path_fits(temp_path_length, _countof(log_file_name))) {
      // Unusable temp path: report failure so the caller can fall back.
      return INVALID_HANDLE_VALUE;
    }
    wcscat_s(log_file_name, LOG_SUFFIX);
    return open_log_handle(log_file_name);
  }

  // A core process left over from a build that opened the log with
  // FILE_SHARE_READ only still blocks the primary name, so fall back to a
  // per-instance file before giving up on logging entirely.
  inline HANDLE
  open_fallback_log_handle() {
    wchar_t temp_path[MAX_PATH];
    const auto temp_path_length = GetTempPathW(_countof(temp_path), temp_path);
    if (fallback_temp_path_usable(temp_path_length, _countof(temp_path))) {
      const auto handle = open_log_handle(
        fallback_log_name(std::wstring(temp_path, temp_path_length), GetCurrentProcessId()).c_str());
      if (handle != INVALID_HANDLE_VALUE) {
        return handle;
      }
    }

    // The temp path is unusable or the per-instance name failed: NUL keeps
    // the service startable even without a log sink.
    SECURITY_ATTRIBUTES security_attributes = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
    return CreateFileW(L"NUL",
      GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE,
      &security_attributes,
      OPEN_EXISTING,
      0,
      NULL);
  }
}  // namespace sunshinesvc
