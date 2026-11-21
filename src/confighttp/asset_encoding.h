#pragma once

#include "src/http/types.h"
#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>

namespace confighttp::assets {
  enum class asset_encoding { identity, gzip, unacceptable };

  inline std::string_view
  trim_encoding(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
    return value;
  }

  inline asset_encoding
  select_asset_encoding(const web::CaseInsensitiveMultimap &headers, bool gzip_available) {
    std::optional<double> gzip, identity, wildcard;
    const auto [first, last] = headers.equal_range("Accept-Encoding");
    for (auto it = first; it != last; ++it) {
      std::string_view remaining = it->second;
      while (!remaining.empty()) {
        const auto comma = remaining.find(',');
        auto item = trim_encoding(remaining.substr(0, comma));
        remaining = comma == std::string_view::npos ? std::string_view {} : remaining.substr(comma + 1);
        const auto semi = item.find(';');
        auto token = trim_encoding(item.substr(0, semi));
        std::string name(token);
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        double quality = 1;
        if (semi != std::string_view::npos) {
          const auto parameter = trim_encoding(item.substr(semi + 1));
          if (parameter.size() < 3 || (parameter[0] != 'q' && parameter[0] != 'Q') || parameter[1] != '=') continue;
          try {
            const std::string number(trim_encoding(parameter.substr(2)));
            size_t consumed = 0;
            quality = std::stod(number, &consumed);
            if (consumed != number.size() || quality < 0 || quality > 1 || !(quality >= 0)) continue;
          } catch (...) { continue; }
        }
        if (name == "gzip") gzip = quality;
        else if (name == "identity") identity = quality;
        else if (name == "*") wildcard = quality;
      }
    }
    const auto gzip_quality = gzip.value_or(wildcard.value_or(0));
    if (gzip_available && gzip_quality > 0 && (!identity || gzip_quality >= *identity)) return asset_encoding::gzip;
    if (identity.value_or(wildcard == 0 ? 0 : 1) > 0) return asset_encoding::identity;
    return asset_encoding::unacceptable;
  }
}  // namespace confighttp::assets
