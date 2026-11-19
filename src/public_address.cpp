/**
 * @file src/public_address.cpp
 * @brief Public address syntax rules shared by the configuration parser,
 *        the reachability check and their unit tests.
 */
#include "public_address.h"

#include <algorithm>
#include <cctype>
#include <cstdint>

#include <boost/asio/ip/address.hpp>
#include <boost/system/error_code.hpp>

namespace public_address {
  namespace {
    bool
    is_space(unsigned char value) {
      return value == ' ' || value == '\t' || value == '\r' || value == '\n' || value == '\f' || value == '\v';
    }

    bool
    is_ip_literal(std::string_view value) {
      if (value.empty()) return false;
      boost::system::error_code error;
      boost::asio::ip::make_address(std::string(value), error);
      return !error;
    }

    // rfc1035-20261103 section 2.3.1: labels of letters, digits and hyphens,
    // at most 63 octets; rfc1123-20261103 section 2.1 relaxes the first
    // character to a digit.
    bool
    is_hostname(std::string_view value) {
      if (value.empty() || value.size() > 253) return false;
      bool all_numeric = true;
      std::size_t label_length = 0;
      for (std::size_t index = 0; index < value.size(); ++index) {
        const auto character = static_cast<unsigned char>(value[index]);
        if (character == '.') {
          if (label_length == 0) return false;
          if (value[index - 1] == '-') return false;
          label_length = 0;
          continue;
        }
        if (!std::isalnum(character) && character != '-') return false;
        if (!std::isdigit(character)) all_numeric = false;
        if (character == '-' && label_length == 0) return false;
        if (++label_length > 63) return false;
      }
      if (label_length == 0) return false;
      if (value.back() == '-') return false;
      // "123.456.789.12" is a malformed address, not a useful host name.
      if (all_numeric) return false;
      return true;
    }
  }  // namespace

  std::string
  trim(std::string_view value) {
    std::size_t first = 0;
    std::size_t last = value.size();
    while (first < last && is_space(static_cast<unsigned char>(value[first]))) ++first;
    while (last > first && is_space(static_cast<unsigned char>(value[last - 1]))) --last;
    return std::string { value.substr(first, last - first) };
  }

  std::string
  normalize(std::string_view value) {
    auto result = trim(value);
    if (!result.empty() && result.back() == '.' && !is_ip_literal(result)) result.pop_back();
    return result;
  }

  kind_e
  classify(std::string_view value) {
    const auto text = normalize(value);
    if (text.empty()) return kind_e::empty;
    if (is_ip_literal(text)) return kind_e::ip;
    if (is_hostname(text)) return kind_e::hostname;
    return kind_e::invalid;
  }

  bool
  is_valid(std::string_view value) {
    const auto kind = classify(value);
    return kind == kind_e::ip || kind == kind_e::hostname;
  }

  std::string_view
  to_string(kind_e value) {
    switch (value) {
      case kind_e::empty:
        return "empty";
      case kind_e::ip:
        return "ip";
      case kind_e::hostname:
        return "hostname";
      case kind_e::invalid:
        return "invalid";
    }
    return "invalid";
  }
  namespace {
    bool
    is_list_separator(char value) {
      return value == ',' || value == ';' || value == '\n' || value == '\r' || value == ' ' || value == '\t';
    }

    // Splits at separators that are outside square brackets, so an IPv6 literal keeps its colons and a
    // list wrapped in one pair of brackets stays one token until the caller unwraps it.
    std::vector<std::string>
    split_list(std::string_view text) {
      std::vector<std::string> tokens;
      std::string current;
      int depth = 0;
      for (const char value : text) {
        if (value == '[') ++depth;
        if (value == ']' && depth > 0) --depth;
        if (depth == 0 && is_list_separator(value)) {
          if (!current.empty()) tokens.push_back(std::move(current));
          current.clear();
          continue;
        }
        current.push_back(value);
      }
      if (!current.empty()) tokens.push_back(std::move(current));
      return tokens;
    }

    // Host part to a normalized endpoint host; sets `error` and returns false when it is neither an IP nor a name.
    bool
    set_host(endpoint_t &endpoint, std::string_view host, endpoint_error_e &error) {
      if (host.find('%') != std::string_view::npos) {
        error = endpoint_error_e::host;
        return false;
      }
      const auto normalized = normalize(host);
      const auto kind = classify(normalized);
      if (kind != kind_e::ip && kind != kind_e::hostname) {
        error = endpoint_error_e::host;
        return false;
      }
      endpoint.kind = kind;
      if (kind == kind_e::ip) {
        boost::system::error_code ignored;
        endpoint.host = boost::asio::ip::make_address(normalized, ignored).to_string();
      }
      else {
        endpoint.host = normalized;
        std::transform(endpoint.host.begin(), endpoint.host.end(), endpoint.host.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      }
      return true;
    }

    bool
    set_port(endpoint_t &endpoint, std::string_view digits, endpoint_error_e &error) {
      if (digits.empty() || digits.size() > 5 || !std::all_of(digits.begin(), digits.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
        error = endpoint_error_e::port;
        return false;
      }
      unsigned long value = 0;
      for (const char digit : digits) value = value * 10 + static_cast<unsigned long>(digit - '0');
      if (value <= HTTPS_PORT_OFFSET || value > 65535) {
        error = endpoint_error_e::port;
        return false;
      }
      endpoint.port = static_cast<std::uint16_t>(value);
      return true;
    }

    bool
    is_ipv6_literal(std::string_view value) {
      boost::system::error_code error;
      return boost::asio::ip::make_address(std::string(value), error).is_v6() && !error;
    }
  }  // namespace

  endpoint_parse_t
  parse_endpoint(std::string_view input) {
    endpoint_parse_t result;
    const auto text = trim(input);
    if (text.empty()) {
      result.error = endpoint_error_e::empty;
      return result;
    }
    if (text.find("://") != std::string::npos) {
      result.error = endpoint_error_e::scheme;
      return result;
    }
    if (std::any_of(text.begin(), text.end(), [](unsigned char c) { return c == '/' || c == '?' || c == '#' || c == '@' || is_space(c); })) {
      result.error = endpoint_error_e::format;
      return result;
    }

    if (text.front() == '[') {
      const auto close = text.find(']');
      if (close == std::string::npos) {
        result.error = endpoint_error_e::format;
        return result;
      }
      const auto host = text.substr(1, close - 1);
      if (!is_ipv6_literal(host) || !set_host(result.endpoint, host, result.error)) {
        result.error = endpoint_error_e::host;
        return result;
      }
      const auto rest = std::string_view(text).substr(close + 1);
      if (rest.empty()) return result;
      if (rest.front() != ':') {
        result.error = endpoint_error_e::format;
        return result;
      }
      set_port(result.endpoint, rest.substr(1), result.error);
      return result;
    }

    const auto colons = std::count(text.begin(), text.end(), ':');
    if (colons == 0) {
      set_host(result.endpoint, text, result.error);
      return result;
    }
    if (colons == 1) {
      const auto at = text.find(':');
      if (!set_host(result.endpoint, std::string_view(text).substr(0, at), result.error)) return result;
      set_port(result.endpoint, std::string_view(text).substr(at + 1), result.error);
      return result;
    }
    // Two colons or more: a bare IPv6 literal. With a port it has to be bracketed.
    if (!is_ipv6_literal(text)) {
      result.error = endpoint_error_e::host;
      return result;
    }
    set_host(result.endpoint, text, result.error);
    return result;
  }

  std::string
  format_endpoint(const endpoint_t &endpoint) {
    std::string text = endpoint.host;
    if (endpoint.host.find(':') != std::string::npos) text = "[" + endpoint.host + "]";
    if (endpoint.port != 0) text += ":" + std::to_string(endpoint.port);
    return text;
  }

  std::uint16_t
  http_port_of(const endpoint_t &endpoint, std::uint16_t default_port) {
    return endpoint.port != 0 ? endpoint.port : default_port;
  }

  std::uint16_t
  https_port_of(std::uint16_t http_port) {
    return http_port > HTTPS_PORT_OFFSET ? static_cast<std::uint16_t>(http_port - HTTPS_PORT_OFFSET) : 0;
  }

  namespace {
    void
    append_tokens(endpoint_list_t &list, const std::vector<std::string> &tokens) {
      for (const auto &token : tokens) {
        const auto parsed = parse_endpoint(token);
        if (!parsed.ok()) {
          list.rejected.push_back(token);
          continue;
        }
        const auto same = [&](const endpoint_t &other) { return other.host == parsed.endpoint.host && other.port == parsed.endpoint.port; };
        if (std::any_of(list.endpoints.begin(), list.endpoints.end(), same)) continue;
        if (list.endpoints.size() >= MAX_ENDPOINTS) {
          list.truncated = true;
          continue;
        }
        list.endpoints.push_back(parsed.endpoint);
      }
    }
  }  // namespace

  endpoint_list_t
  parse_endpoint_list(std::string_view input) {
    endpoint_list_t list;
    const auto text = trim(input);
    append_tokens(list, split_list(text));
    // `[a, b]`: the same list in the bracketed form other list keys use.
    if (!list.rejected.empty() && text.size() >= 2 && text.front() == '[' && text.back() == ']') {
      endpoint_list_t inner;
      append_tokens(inner, split_list(std::string_view(text).substr(1, text.size() - 2)));
      if (inner.rejected.empty()) return inner;
    }
    return list;
  }

  std::string
  join_endpoints(const std::vector<endpoint_t> &endpoints) {
    std::string text;
    for (const auto &endpoint : endpoints) {
      if (!text.empty()) text += ", ";
      text += format_endpoint(endpoint);
    }
    return text;
  }

  endpoint_list_t
  effective_endpoints(std::string_view endpoints_text, std::string_view legacy_external_ip) {
    auto list = parse_endpoint_list(endpoints_text);
    if (!list.endpoints.empty()) return list;
    // No usable external_endpoints: honour the single value older configurations carry.
    auto legacy = parse_endpoint_list(legacy_external_ip);
    legacy.rejected.insert(legacy.rejected.begin(), list.rejected.begin(), list.rejected.end());
    return legacy;
  }
}  // namespace public_address
