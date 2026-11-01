/**
 * @file src/platform/windows/text_encoding.cpp
 * @brief Windows text encoding helpers shared by small host tools.
 *
 * Authority: ds5-ms-multibytetowidechar-20261102 (MultiByteToWideChar);
 * ds5-ms-widechartomultibyte-20261102 (WideCharToMultiByte).
 */

#include "misc.h"

#include <string>
#include <string_view>

namespace platf {

  // Standalone tools link this file without the logging sources, so a failure is
  // reported through a hook that misc.cpp installs in the host binaries.
  void (*text_encoding_failure_hook)(const char *what, DWORD error) = nullptr;

  namespace {
    void
    report_failure(const char *what) {
      const DWORD error = GetLastError();
      if (text_encoding_failure_hook) {
        text_encoding_failure_hook(what, error);
      }
    }
  }  // namespace

  std::wstring
  from_utf8(const std::string &string) {
    if (string.empty()) {
      return {};
    }

    auto output_size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, string.data(),
      static_cast<int>(string.size()), nullptr, 0);
    if (output_size == 0) {
      report_failure("Failed to get UTF-16 buffer size");
      return {};
    }

    std::wstring output(output_size, L'\0');
    output_size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, string.data(),
      static_cast<int>(string.size()), output.data(), output.size());
    if (output_size == 0) {
      report_failure("Failed to convert string to UTF-16");
      return {};
    }

    return output;
  }

  std::string
  to_utf8(const std::wstring &string) {
    if (string.empty()) {
      return {};
    }

    auto output_size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, string.data(),
      static_cast<int>(string.size()), nullptr, 0, nullptr, nullptr);
    if (output_size == 0) {
      report_failure("Failed to get UTF-8 buffer size");
      return {};
    }

    std::string output(output_size, '\0');
    output_size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, string.data(),
      static_cast<int>(string.size()), output.data(), output.size(), nullptr, nullptr);
    if (output_size == 0) {
      report_failure("Failed to convert string to UTF-8");
      return {};
    }

    return output;
  }

  std::string
  to_utf8(std::string_view text, UINT code_page) {
    if (text.empty()) {
      return {};
    }

    const DWORD strict_flags = (code_page == CP_UTF8 || code_page == 54936) ? MB_ERR_INVALID_CHARS : 0;
    const auto *source = text.data();
    const int source_size = static_cast<int>(text.size());

    int wide_size = MultiByteToWideChar(code_page, strict_flags, source, source_size, nullptr, 0);
    DWORD flags = strict_flags;
    if (wide_size <= 0) {
      flags = 0;
      wide_size = MultiByteToWideChar(code_page, flags, source, source_size, nullptr, 0);
    }
    if (wide_size <= 0) {
      return {};
    }

    std::wstring wide(wide_size, L'\0');
    int converted = MultiByteToWideChar(code_page, flags, source, source_size, wide.data(), wide_size);
    if (converted <= 0 && flags != 0) {
      flags = 0;
      converted = MultiByteToWideChar(code_page, flags, source, source_size, wide.data(), wide_size);
    }
    if (converted <= 0) {
      return {};
    }
    wide.resize(converted);
    return to_utf8(wide);
  }

  bool
  is_valid_utf8(std::string_view text) {
    if (text.empty()) {
      return true;
    }
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0) > 0;
  }

  std::string
  to_utf8_best_effort(std::string_view text, UINT fallback_code_page) {
    if (is_valid_utf8(text)) {
      return std::string { text };
    }
    auto converted = to_utf8(text, fallback_code_page);
    if (!converted.empty()) {
      return converted;
    }
    return to_utf8(text, CP_UTF8);
  }

}  // namespace platf
