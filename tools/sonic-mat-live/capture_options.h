#pragma once
#include <cstdint>
#include <stdexcept>
#include <string_view>
namespace sonic_live {
  // A missing or malformed bound must never accidentally enable continuous capture.
  inline uint64_t
  capture_limit(int count, const wchar_t *option, const wchar_t *value = nullptr) {
    if (count == 1 && option && std::wstring_view(option) == L"--continuous") return 0;
    if (count != 2 || !option || std::wstring_view(option) != L"--max-seconds" || !value || !*value)
      throw std::runtime_error("Explicit --continuous or --max-seconds 1..60 required");
    uint64_t result = 0;
    for (auto p = value; *p; ++p) {
      if (*p < L'0' || *p > L'9' || result > 60) throw std::runtime_error("Capture duration 1..60 seconds");
      result = result * 10 + (*p - L'0');
    }
    if (!result || result > 60) throw std::runtime_error("Capture duration 1..60 seconds");
    return result;
  }
}  // namespace sonic_live
