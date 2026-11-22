#pragma once

#include "src/http/types.h"
#include "src/http/etag.h"
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
      if (web::etag_matches(it->second, etag)) return true;
    }
    return false;
  }
}  // namespace confighttp::assets
