/**
 * @file src/public_address.h
 * @brief Validation of the operator-supplied public address (config key external_ip).
 *
 * The value is either an IP literal or a DNS host name (for example a dynamic
 * DNS name). The rules follow RFC 1123 section 2.1 (host name syntax updated
 * from RFC 952) and RFC 1035 section 2.3.1 (preferred name syntax).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace public_address {
  enum class kind_e {
    empty,
    ip,
    hostname,
    invalid,
  };

  /// Remove leading and trailing ASCII whitespace.
  std::string
  trim(std::string_view value);

  /// Drop one trailing root dot from a host name; leaves IP literals alone.
  std::string
  normalize(std::string_view value);

  /// Classify a configured value. RFC 1123 host names and IP literals are valid.
  kind_e
  classify(std::string_view value);

  /// True when classify() is ip or hostname.
  bool
  is_valid(std::string_view value);

  std::string_view
  to_string(kind_e value);

  // ---- public endpoints (config key external_endpoints) ------------------
  //
  // A public endpoint is written the way a host is added in Moonlight: a host
  // name, an IPv4 address or an IPv6 address, with an optional port. The port is
  // the HTTP port the client is given (the base port as the router sees it); the
  // HTTPS pairing port is five below it. IPv6 literals with a port are bracketed
  // (RFC 3986 section 3.2.2): [2001:db8::1]:57989.

  /// At most this many endpoints are kept and probed.
  constexpr std::size_t MAX_ENDPOINTS = 8;

  /// The HTTPS port is the HTTP port minus this offset.
  constexpr std::uint16_t HTTPS_PORT_OFFSET = 5;

  enum class endpoint_error_e {
    none,
    empty,
    scheme,  ///< a URL scheme such as https://
    format,  ///< a path, a space or anything else that is not host[:port]
    host,  ///< the host part is neither an IP literal nor a valid host name
    port,  ///< not a number from HTTPS_PORT_OFFSET + 1 to 65535
  };

  struct endpoint_t {
    std::string host;  ///< Normalized host: lower-case IPv6, no root dot, no brackets.
    std::uint16_t port = 0;  ///< 0 when the text gave no port.
    kind_e kind = kind_e::empty;  ///< ip or hostname
  };

  struct endpoint_parse_t {
    endpoint_t endpoint;
    endpoint_error_e error = endpoint_error_e::none;
    bool
    ok() const {
      return error == endpoint_error_e::none;
    }
  };

  /// Parse one endpoint: `host`, `host:port`, `a.b.c.d[:port]`, `[v6][:port]` or a bare IPv6 literal.
  endpoint_parse_t
  parse_endpoint(std::string_view text);

  /// Canonical text of an endpoint: `host`, `host:port`, `[v6]` or `[v6]:port`.
  std::string
  format_endpoint(const endpoint_t &endpoint);

  /// The HTTP port an endpoint stands for: its own, or `default_port` when it gave none.
  std::uint16_t
  http_port_of(const endpoint_t &endpoint, std::uint16_t default_port);

  /// HTTPS port of an HTTP port (five below); 0 when that is not a valid port.
  std::uint16_t
  https_port_of(std::uint16_t http_port);

  struct endpoint_list_t {
    std::vector<endpoint_t> endpoints;  ///< valid, de-duplicated, at most MAX_ENDPOINTS
    std::vector<std::string> rejected;  ///< items that did not parse (text as given)
    bool truncated = false;  ///< more than MAX_ENDPOINTS valid items were given
  };

  /// Parse a list separated by commas, semicolons, spaces or new lines; one pair of outer brackets is accepted.
  endpoint_list_t
  parse_endpoint_list(std::string_view text);

  /// Canonical list text stored in the configuration: `a:1, [v6]:2`.
  std::string
  join_endpoints(const std::vector<endpoint_t> &endpoints);

  /**
   * @brief The endpoints in force: `external_endpoints` when it names any, otherwise
   *        the single legacy `external_ip` value (compatibility with older configurations).
   */
  endpoint_list_t
  effective_endpoints(std::string_view endpoints_text, std::string_view legacy_external_ip);
}  // namespace public_address
