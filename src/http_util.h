/**
 * @file src/http_util.h
 * @brief Small shared helpers for HTTP request validation.
 */
#pragma once

#include <string>
#include <string_view>

#include <boost/algorithm/string.hpp>

namespace http_util {
  inline bool
  content_type_matches(std::string actual, std::string_view expected) {
    if (const auto semicolon = actual.find(';'); semicolon != std::string::npos) {
      actual.erase(semicolon);
    }

    auto normalized_expected = std::string { expected };
    boost::algorithm::trim(actual);
    boost::algorithm::to_lower(actual);
    boost::algorithm::to_lower(normalized_expected);
    return actual == normalized_expected;
  }
}  // namespace http_util
