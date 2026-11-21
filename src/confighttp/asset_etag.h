#pragma once

#include "src/http/types.h"
#include "src/crypto.h"
#include <string>
#include <string_view>

namespace confighttp::assets {
  inline std::string
  asset_digest(std::string_view content) {
    return util::hex(crypto::hash(content), true).to_string();
  }

  inline std::string
  asset_sidecar_marker(std::string_view source, std::string_view compressed) {
    return asset_digest(source) + " " + asset_digest(compressed) + "\n";
  }

  // GET uses weak comparison, including repeated header fields. Our tags are
  // quoted hexadecimal digests and cannot themselves contain a comma.
  inline bool
  matches_asset_etag(const web::CaseInsensitiveMultimap &headers, std::string_view etag) {
    const auto [first, last] = headers.equal_range("If-None-Match");
    for (auto it = first; it != last; ++it) {
      std::string_view value = it->second;
      while (!value.empty()) {
        const auto comma = value.find(',');
        auto item = value.substr(0, comma);
        value = comma == std::string_view::npos ? std::string_view {} : value.substr(comma + 1);
        while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) item.remove_prefix(1);
        while (!item.empty() && (item.back() == ' ' || item.back() == '\t')) item.remove_suffix(1);
        if (item == "*") return true;
        if (item.substr(0, 2) == "W/") item.remove_prefix(2);
        if (item == etag) return true;
      }
    }
    return false;
  }
}  // namespace confighttp::assets
