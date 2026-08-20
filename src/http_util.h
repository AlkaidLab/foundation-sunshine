/**
 * @file src/http_util.h
 * @brief Small shared helpers for HTTP request validation.
 */
#pragma once

#include <algorithm>
#include <string>
#include <string_view>

namespace http_util {
  inline bool
  content_type_matches(std::string actual, std::string_view expected) {
    if (const auto semicolon = actual.find(';'); semicolon != std::string::npos) {
      actual.erase(semicolon);
    }

    const auto first = actual.find_first_not_of(" \t\r\n");
    const auto last = actual.find_last_not_of(" \t\r\n");
    actual = first == std::string::npos ? std::string {} : actual.substr(first, last - first + 1);

    auto normalized_expected = std::string { expected };
    const auto lowercase_ascii = [](unsigned char value) {
      return static_cast<char>(value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value);
    };
    std::transform(actual.begin(), actual.end(), actual.begin(), lowercase_ascii);
    std::transform(normalized_expected.begin(), normalized_expected.end(), normalized_expected.begin(), lowercase_ascii);
    return actual == normalized_expected;
  }
}  // namespace http_util
