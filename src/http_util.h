/**
 * @file src/http_util.h
 * @brief Small shared helpers for HTTP request validation.
 */
#pragma once

#include <algorithm>
#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace http_util {
  namespace detail {
    inline std::string_view
    trim_ascii_whitespace(std::string_view value) {
      const auto first = value.find_first_not_of(" \t\r\n");
      if (first == std::string_view::npos) {
        return {};
      }
      return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
    }

    inline std::string
    lowercase_ascii(std::string_view value) {
      std::string normalized { value };
      std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char character) {
        return static_cast<char>(character >= 'A' && character <= 'Z' ? character + ('a' - 'A') : character);
      });
      return normalized;
    }

    struct parsed_origin_t {
      std::string scheme;
      std::string authority;
      std::string host;
    };

    inline std::optional<parsed_origin_t>
    parse_origin(std::string_view value) {
      value = trim_ascii_whitespace(value);
      const auto scheme_end = value.find("://");
      if (scheme_end == std::string_view::npos || scheme_end == 0) {
        return std::nullopt;
      }

      const auto authority_begin = scheme_end + 3;
      const auto authority_end = value.find_first_of("/?#", authority_begin);
      const auto authority_view = value.substr(
        authority_begin,
        authority_end == std::string_view::npos ? std::string_view::npos : authority_end - authority_begin
      );
      if (authority_view.empty() || authority_view.find('@') != std::string_view::npos) {
        return std::nullopt;
      }

      std::string_view host_view;
      if (authority_view.front() == '[') {
        const auto bracket = authority_view.find(']');
        if (bracket == std::string_view::npos || bracket == 1) {
          return std::nullopt;
        }
        const auto suffix = authority_view.substr(bracket + 1);
        if (!suffix.empty() && suffix.front() != ':') {
          return std::nullopt;
        }
        host_view = authority_view.substr(1, bracket - 1);
      }
      else {
        const auto first_colon = authority_view.find(':');
        const auto last_colon = authority_view.rfind(':');
        if (first_colon != std::string_view::npos && first_colon != last_colon) {
          return std::nullopt;
        }
        host_view = authority_view.substr(0, first_colon);
      }
      if (host_view.empty()) {
        return std::nullopt;
      }

      auto host = lowercase_ascii(host_view);
      if (host.ends_with('.')) {
        host.pop_back();
      }
      return parsed_origin_t {
        .scheme = lowercase_ascii(value.substr(0, scheme_end)),
        .authority = lowercase_ascii(authority_view),
        .host = std::move(host),
      };
    }

    inline bool
    is_loopback_host(const std::string &host) {
      if (host == "localhost" || host.ends_with(".localhost")) {
        return true;
      }
      if (host == "::1" || host == "0:0:0:0:0:0:0:1") {
        return true;
      }

      std::size_t begin = 0;
      unsigned int first_octet = 0;
      for (int index = 0; index < 4; ++index) {
        const auto end = index == 3 ? host.size() : host.find('.', begin);
        if (end == std::string::npos || end == begin) {
          return false;
        }

        unsigned int octet = 0;
        const auto *first = host.data() + begin;
        const auto *last = host.data() + end;
        const auto [parsed, error] = std::from_chars(first, last, octet);
        if (error != std::errc {} || parsed != last || octet > 255) {
          return false;
        }
        if (index == 0) {
          first_octet = octet;
        }
        begin = end + 1;
      }
      return first_octet == 127;
    }

    inline bool
    is_supported_browser_url(std::string_view value) {
      const auto origin = parse_origin(value);
      return origin && (origin->scheme == "http" || origin->scheme == "https" || origin->scheme == "tauri");
    }

    inline bool
    is_trusted_browser_url(
      std::string_view value,
      std::string_view request_host,
      const bool allow_loopback_origin
    ) {
      const auto origin = parse_origin(value);
      if (!origin || (origin->scheme != "http" && origin->scheme != "https" && origin->scheme != "tauri")) {
        return false;
      }
      if (is_loopback_host(origin->host)) {
        return allow_loopback_origin;
      }

      return origin->scheme == "https" &&
             origin->authority == lowercase_ascii(trim_ascii_whitespace(request_host));
    }
  }  // namespace detail

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

  /**
   * Validate browser provenance without rejecting native API clients that do
   * not send browser-only request headers.
   */
  inline bool
  browser_request_source_allowed(
    const std::optional<std::string_view> origin,
    const std::optional<std::string_view> referer,
    const std::optional<std::string_view> fetch_site,
    const std::string_view request_host,
    const bool allow_loopback_origin
  ) {
    if (fetch_site) {
      const auto normalized_fetch_site = detail::lowercase_ascii(detail::trim_ascii_whitespace(*fetch_site));
      if (normalized_fetch_site == "same-origin") {
        return (!origin || detail::is_supported_browser_url(*origin)) &&
               (!referer || detail::is_supported_browser_url(*referer));
      }
      if (normalized_fetch_site == "none" && !origin && !referer) {
        return true;
      }
      if (origin && detail::is_trusted_browser_url(*origin, request_host, allow_loopback_origin)) {
        return true;
      }
      if (!origin && referer && detail::is_trusted_browser_url(*referer, request_host, allow_loopback_origin)) {
        return true;
      }
      return false;
    }

    if (origin) {
      return detail::is_trusted_browser_url(*origin, request_host, allow_loopback_origin);
    }
    if (referer) {
      return detail::is_trusted_browser_url(*referer, request_host, allow_loopback_origin);
    }
    return true;
  }
}  // namespace http_util
