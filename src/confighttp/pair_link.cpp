/**
 * @file src/confighttp/pair_link.cpp
 * @brief Choose the host and port placed in a moonlight://pair link.
 *
 * See pair_link.h for the source records this file relies on.
 */
#include "pair_link.h"

#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <cctype>

namespace confighttp::pair_link {
  namespace ip = boost::asio::ip;

  namespace {
    enum class reach_e {
      reject,
      name,
      lan,
      global,
    };

    struct examined_t {
      reach_e reach = reach_e::reject;
      std::string text;
      int rank = 100;
    };

    std::string_view
    trim(std::string_view value) {
      while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.remove_prefix(1);
      while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.remove_suffix(1);
      return value;
    }

    bool
    equals_ignore_case(std::string_view left, std::string_view right) {
      if (left.size() != right.size()) return false;
      for (std::size_t i = 0; i < left.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(left[i])) != std::tolower(static_cast<unsigned char>(right[i]))) return false;
      }
      return true;
    }

    bool
    is_hostname(std::string_view value) {
      if (value.empty() || value.size() > 253) return false;
      if (value.front() == '.' || value.back() == '.') return false;
      bool label = false;
      int label_length = 0;
      for (unsigned char c : value) {
        if (c == '.') {
          if (!label || label_length > 63) return false;
          label = false;
          label_length = 0;
          continue;
        }
        if (!(std::isalnum(c) || c == '-')) return false;
        if (!label && c == '-') return false;
        label = true;
        ++label_length;
      }
      return label && label_length <= 63;
    }

    int
    lan_rank(const ip::address &address) {
      if (address.is_v4()) {
        const auto bytes = address.to_v4().to_bytes();
        if (bytes[0] == 192 && bytes[1] == 168) return 0;
        if (bytes[0] == 10) return 1;
        if (bytes[0] == 172 && bytes[1] >= 16 && bytes[1] <= 31) return 2;
        if (bytes[0] == 100 && bytes[1] >= 64 && bytes[1] <= 127) return 3;
        if (bytes[0] == 169 && bytes[1] == 254) return 4;
        return 100;
      }
      const auto bytes = address.to_v6().to_bytes();
      if ((bytes[0] & 0xfe) == 0xfc) return 5;
      return 100;
    }

    examined_t
    examine(std::string_view raw, bool allow_name) {
      raw = trim(raw);
      if (raw.size() >= 2 && raw.front() == '[' && raw.back() == ']') raw = raw.substr(1, raw.size() - 2);
      if (raw.empty() || raw == "*" || equals_ignore_case(raw, "localhost") || equals_ignore_case(raw, "localhost.localdomain")) {
        return {};
      }

      boost::system::error_code error;
      auto address = ip::make_address(raw, error);
      if (error) {
        if (allow_name && is_hostname(raw)) return { reach_e::name, std::string { raw }, 100 };
        return {};
      }
      if (address.is_v6() && address.to_v6().is_v4_mapped()) {
        address = ip::make_address_v4(ip::v4_mapped, address.to_v6());
      }
      if (address.is_unspecified() || address.is_loopback() || address.is_multicast()) return {};
      if (address.is_v4() && (address.to_v4().to_bytes()[0] == 0 || address.to_v4() == ip::address_v4::broadcast())) return {};
      if (address.is_v6() && address.to_v6().is_link_local()) return {};

      const auto rank = lan_rank(address);
      return { rank < 100 ? reach_e::lan : reach_e::global, address.to_string(), rank };
    }

    std::string
    authority_host(std::string_view authority) {
      authority = trim(authority);
      if (authority.empty()) return {};
      if (authority.front() == '[') {
        const auto end = authority.find(']');
        if (end == std::string_view::npos || end < 2) return {};
        return std::string { authority.substr(1, end - 1) };
      }
      const auto colon = authority.rfind(':');
      if (colon != std::string_view::npos && authority.find(':') == colon) {
        const auto port = authority.substr(colon + 1);
        const auto digits = !port.empty() && std::all_of(port.begin(), port.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
        if (digits) return std::string { authority.substr(0, colon) };
      }
      return std::string { authority };
    }

    examined_t
    best_lan(const std::vector<std::string> &addresses) {
      examined_t best;
      for (const auto &address : addresses) {
        const auto examined = examine(address, false);
        if (examined.reach != reach_e::lan) continue;
        if (best.reach != reach_e::lan || examined.rank < best.rank) best = examined;
      }
      return best;
    }

    examined_t
    best_global(const std::vector<std::string> &addresses) {
      examined_t best;
      for (const auto &address : addresses) {
        const auto examined = examine(address, false);
        if (examined.reach != reach_e::global) continue;
        const auto better = best.reach != reach_e::global || (examined.text.find(':') == std::string::npos && best.text.find(':') != std::string::npos);
        if (better) best = examined;
      }
      return best;
    }
  }  // namespace

  target_t
  select_target(const request_t &request) {
    target_t target;
    target.port = request.http_port;

    const auto external = examine(request.external_host, true);
    if (external.reach == reach_e::name || external.reach == reach_e::lan || external.reach == reach_e::global) {
      target.host = external.text;
      if (request.external_port != 0) target.port = request.external_port;
      return target;
    }

    const auto socket = examine(request.socket_address, false);
    if (socket.reach == reach_e::lan || socket.reach == reach_e::global) {
      target.host = socket.text;
      return target;
    }

    const auto header = examine(authority_host(request.host_header), false);
    if (header.reach == reach_e::lan || header.reach == reach_e::global) {
      target.host = header.text;
      return target;
    }

    const auto route = examine(request.default_route_address, false);
    if (route.reach == reach_e::lan || route.reach == reach_e::global) {
      target.host = route.text;
      return target;
    }

    const auto lan = best_lan(request.interface_addresses);
    if (lan.reach == reach_e::lan) {
      target.host = lan.text;
      return target;
    }

    const auto global = best_global(request.interface_addresses);
    if (global.reach == reach_e::global) target.host = global.text;
    return target;
  }

  std::string
  query_escape(std::string_view value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size());
    for (unsigned char c : value) {
      if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~' || c == ':') {
        encoded.push_back(static_cast<char>(c));
      }
      else {
        encoded.push_back('%');
        encoded.push_back(hex[c >> 4]);
        encoded.push_back(hex[c & 0x0f]);
      }
    }
    return encoded;
  }

  std::string
  pair_url(const target_t &target, std::string_view pin, std::string_view name) {
    if (target.host.empty() || target.port == 0) return {};
    return "moonlight://pair?host=" + query_escape(target.host) +
           "&port=" + std::to_string(target.port) +
           "&pin=" + query_escape(pin) +
           "&name=" + query_escape(name);
  }

  std::string
  default_route_address() {
    try {
      boost::asio::io_context context;
      ip::udp::socket socket { context };
      boost::system::error_code error;
      // 192.0.2.1 is documentation range (TEST-NET-1). No datagram is sent:
      // UDP connect only asks the kernel which source the default route uses.
      socket.connect(ip::udp::endpoint { ip::make_address("192.0.2.1"), 9 }, error);
      if (error) return {};
      const auto local = socket.local_endpoint(error);
      if (error) return {};
      const auto examined = examine(local.address().to_string(), false);
      if (examined.reach == reach_e::lan || examined.reach == reach_e::global) return examined.text;
    }
    catch (...) {
    }
    return {};
  }
}  // namespace confighttp::pair_link
