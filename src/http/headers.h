/**
 * @file src/http/headers.h
 * @brief Header validation shared by Beast request and response serializers.
 */
#pragma once
#include "types.h"
#include <algorithm>
#include <stdexcept>
namespace web::detail {
  template <class Message>
  void
  apply_headers(Message &message, const CaseInsensitiveMultimap &values) {
    for (const auto &[name, value] : values) {
      const auto token = [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
      };
      const auto control = [](unsigned char c) { return (c < 0x20 && c != '\t') || c == 0x7f; };
      if (name.empty() || !std::all_of(name.begin(), name.end(), token) || std::any_of(value.begin(), value.end(), control)) throw std::invalid_argument("Invalid HTTP header");
      message.insert(name, value);
    }
  }
}  // namespace web::detail
