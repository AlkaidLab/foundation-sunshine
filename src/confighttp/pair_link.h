/**
 * @file src/confighttp/pair_link.h
 * @brief Reachable host and GameStream HTTP port for a moonlight://pair link.
 *
 * Moonlight V+ Android reads moonlight://pair?host=&port=&pin=&name= and opens
 * host:port as the GameStream HTTP address (default 47989 when port is absent).
 * Desktop Moonlight uses that same HTTP port when a moonlight:// address carries
 * one. The port in the link is therefore the configured base port, never the
 * Web UI port from the browser's Host header.
 *
 * The host is an address the client can open. The unspecified addresses
 * 0.0.0.0 and :: are bind-all, not destinations, and loopback is not reachable
 * from another device. When the browser reached this page on a real address,
 * that address is used. Otherwise the address on the default-route interface
 * is used, then another local LAN address.
 *
 * Sources (docs/authority/source-index.tsv):
 *   pair-link-vplus-qr-20261109
 *   pair-link-moonlight-qt-http-port-20261109
 *   pair-link-android-uri-20261109
 *   pair-link-rfc1122-unspecified-20261109 section 3.2.1.3 (a)
 *   pair-link-rfc4291-unspecified-20261109 sections 2.5.2 and 2.5.3
 *   pair-link-rfc9110-host-20261109 section 7.2
 *   pair-link-rfc3986-query-20261109 sections 2.3 and 3.4
 *   pair-link-rfc1918-20261109
 *   pair-link-rfc6598-shared-20261109
 *   pair-link-rfc4193-ula-20261109 section 3.1
 *   pair-link-rfc5737-testnet-20261109
 *   pair-link-connect-20261109
 *   pair-link-getsockname-20261109
 *   pair-link-win-connect-20261109
 *   pair-link-getifaddrs-20261109
 *   pair-link-getadaptersaddresses-20261109
 */
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace confighttp::pair_link {
  struct request_t {
    /// Local address of the accepted Web UI socket. May be 0.0.0.0 or ::.
    std::string socket_address;
    /// Raw Host header (RFC 9110 section 7.2), including a port if present.
    std::string host_header;
    /// GameStream HTTP port: the configured base port plus the HTTP offset (0).
    std::uint16_t http_port = 0;
    /// Operator-supplied public endpoint host, or empty.
    std::string external_host;
    /// Operator-supplied HTTP port. 0 means http_port.
    std::uint16_t external_port = 0;
    /// Unicast addresses currently assigned on this host.
    std::vector<std::string> interface_addresses;
    /// Source address the kernel selects for the default route, or empty.
    std::string default_route_address;
  };

  struct target_t {
    std::string host;
    std::uint16_t port = 0;
  };

  target_t
  select_target(const request_t &request);

  /// moonlight://pair URL, or empty when no reachable host or port is known.
  /// Query values are percent-encoded except unreserved characters and ':'.
  std::string
  pair_url(const target_t &target, std::string_view pin, std::string_view name);

  /// Local address of the interface used for the default route.
  /// connect() on a UDP socket does not transmit; getsockname reads the source
  /// the kernel bound. Empty when the host has no such route.
  std::string
  default_route_address();
}  // namespace confighttp::pair_link
