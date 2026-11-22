#pragma once

#include "src/http/types.h"
#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>

namespace confighttp::assets {
  enum class asset_encoding { identity,
    gzip,
    brotli,
    unacceptable };

  // What this URL can actually be served from: the shipped Brotli copy, a
  // legacy gzip sidecar that still matches its source, and an identity
  // representation (the plain file, or an on-the-fly decode of the copy).
  struct asset_encoding_offers {
    bool brotli = false;
    bool gzip = false;
    bool identity = false;
  };

  inline std::string_view
  trim_encoding(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
    return value;
  }

  // RFC 9110 section 12.5.3: a coding is acceptable when it is named with a
  // positive quality, or unnamed while the wildcard is positive; identity is
  // additionally acceptable by default. Among the acceptable codings the host
  // can serve, the smallest transfer wins a tie.
  inline asset_encoding
  select_asset_encoding(const web::CaseInsensitiveMultimap &headers, const asset_encoding_offers &offers) {
    std::optional<double> brotli, gzip, identity, wildcard;
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
          }
          catch (...) {
            continue;
          }
        }
        if (name == "br")
          brotli = quality;
        else if (name == "gzip")
          gzip = quality;
        else if (name == "identity")
          identity = quality;
        else if (name == "*")
          wildcard = quality;
      }
    }
    const auto quality = [&wildcard](const std::optional<double> &named, bool acceptable_by_default) {
      if (named) return *named;
      if (wildcard) return *wildcard;
      return acceptable_by_default ? 1.0 : 0.0;
    };
    // An explicitly named coding outranks the implicit identity default, and
    // an unnamed coding competes only through the wildcard; the default alone
    // just keeps identity acceptable when nothing else was chosen. This is
    // the behaviour the shipped parser established.
    const auto competes = [&wildcard](const std::optional<double> &named) -> std::optional<double> {
      if (named) return *named > 0 ? named : std::nullopt;
      return wildcard && *wildcard > 0 ? wildcard : std::nullopt;
    };
    struct coding_candidate {
      asset_encoding encoding;
      bool offered;
      std::optional<double> client_quality;
    };
    const coding_candidate candidates[] = {
      { asset_encoding::brotli, offers.brotli, competes(brotli) },
      { asset_encoding::gzip, offers.gzip, competes(gzip) },
      { asset_encoding::identity, offers.identity, competes(identity) },
    };
    const coding_candidate *best = nullptr;
    for (const auto &candidate : candidates) {
      if (!candidate.offered || !candidate.client_quality) continue;
      if (!best || *candidate.client_quality > *best->client_quality) best = &candidate;
    }
    if (best) return best->encoding;
    if (offers.identity && quality(identity, true) > 0) return asset_encoding::identity;
    return asset_encoding::unacceptable;
  }
}  // namespace confighttp::assets
