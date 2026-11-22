/**
 * @file src/http/etag.h
 * @brief HTTP entity-tag comparison for conditional requests.
 */
#pragma once

#include <string_view>

namespace web {
  // The tags emitted by our callers cannot contain commas. If-None-Match
  // uses weak comparison and can contain a list or the wildcard alone.
  inline bool
  etag_matches(std::string_view if_none_match, std::string_view etag) {
    const auto trim = [](std::string_view value) {
      while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
      while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
      return value;
    };
    if_none_match = trim(if_none_match);
    if (if_none_match == "*") return true;
    if (etag.substr(0, 2) == "W/") etag.remove_prefix(2);
    for (std::size_t entry_begin = 0; entry_begin <= if_none_match.size();) {
      auto entry = if_none_match.substr(entry_begin, if_none_match.find(',', entry_begin) - entry_begin);
      entry = trim(entry);
      if (entry.substr(0, 2) == "W/") entry.remove_prefix(2);
      if (entry == etag) return true;
      const auto next_comma = if_none_match.find(',', entry_begin);
      if (next_comma == std::string_view::npos) break;
      entry_begin = next_comma + 1;
    }
    return false;
  }
}  // namespace web
