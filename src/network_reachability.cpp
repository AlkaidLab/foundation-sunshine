/**
 * @file src/network_reachability.cpp
 * @brief Verdict logic for the host reachability assessment.
 *
 * This translation unit deliberately depends only on the standard library,
 * Boost.Asio address types and nlohmann/json so the verdict rules can be
 * exercised with fixtures without a running host.
 */
#include "network_reachability.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <future>
#include <mutex>

#include <boost/asio/ip/address.hpp>
#include <boost/system/error_code.hpp>

namespace reachability {
  namespace {
    using boost::asio::ip::address;
    using boost::asio::ip::address_v4;
    using boost::asio::ip::address_v6;

    constexpr std::array<std::uint8_t, 4> v4_10 { 10, 0, 0, 0 };
    // IPv4 private and shared ranges: rfc1918-20261103, rfc6598-20261103.
    constexpr std::array<std::uint8_t, 4> v4_100_64 { 100, 64, 0, 0 };
    constexpr std::array<std::uint8_t, 4> v4_127 { 127, 0, 0, 0 };
    constexpr std::array<std::uint8_t, 4> v4_169_254 { 169, 254, 0, 0 };
    constexpr std::array<std::uint8_t, 4> v4_172_16 { 172, 16, 0, 0 };
    constexpr std::array<std::uint8_t, 4> v4_192_168 { 192, 168, 0, 0 };
    constexpr std::array<std::uint8_t, 4> v4_10_144_144 { 10, 144, 144, 0 };
    constexpr std::array<std::uint8_t, 4> v4_multicast { 224, 0, 0, 0 };
    constexpr std::array<std::uint8_t, 4> v4_reserved { 240, 0, 0, 0 };

    constexpr std::array<std::uint8_t, 16> v6_link_local { 0xfe, 0x80 };
    constexpr std::array<std::uint8_t, 16> v6_unique_local { 0xfc };
    constexpr std::array<std::uint8_t, 16> v6_multicast { 0xff };
    // ULA prefix: rfc4193-20261103. Mesh ranges: tailscale-100x-20261103.
    constexpr std::array<std::uint8_t, 16> v6_mesh { 0xfd, 0x7a, 0x11, 0x5c, 0xa1, 0xe0 };

    template <std::size_t N>
    bool
    prefix_match(const std::array<std::uint8_t, N> &bytes, const std::array<std::uint8_t, N> &prefix, int bits) {
      const auto whole = static_cast<std::size_t>(bits / 8);
      const auto remainder = bits % 8;
      for (std::size_t i = 0; i < whole; ++i) {
        if (bytes[i] != prefix[i]) return false;
      }
      if (remainder == 0) return true;
      const auto mask = static_cast<std::uint8_t>(0xffU << (8 - remainder));
      return (bytes[whole] & mask) == (prefix[whole] & mask);
    }

    std::optional<address>
    parse_address(std::string_view value) {
      boost::system::error_code error;
      const auto parsed = boost::asio::ip::make_address(std::string(value), error);
      if (error) return std::nullopt;
      return parsed;
    }

    bool
    is_unspecified(const address &value) {
      return value.is_v4() ? value.to_v4() == address_v4() : value.to_v6() == address_v6();
    }

    bool
    is_public_v4(const address_v4 &value) {
      const auto bytes = value.to_bytes();
      if (is_unspecified(value)) return false;
      if (prefix_match(bytes, v4_127, 8)) return false;
      if (prefix_match(bytes, v4_169_254, 16)) return false;
      if (prefix_match(bytes, v4_10, 8)) return false;
      if (prefix_match(bytes, v4_172_16, 12)) return false;
      if (prefix_match(bytes, v4_192_168, 16)) return false;
      if (prefix_match(bytes, v4_multicast, 4)) return false;
      if (prefix_match(bytes, v4_reserved, 4)) return false;
      return true;
    }

    bool
    is_public_v6(const address_v6 &value) {
      const auto bytes = value.to_bytes();
      if (is_unspecified(value)) return false;
      if (value.is_loopback()) return false;
      if (prefix_match(bytes, v6_link_local, 10)) return false;
      if (prefix_match(bytes, v6_unique_local, 7)) return false;
      if (prefix_match(bytes, v6_multicast, 8)) return false;
      if (value.is_v4_mapped()) return is_public_v4(boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, value));
      return true;
    }

    /// Mirror of src/network.cpp address classification for the Web UI origin policy.
    int
    origin_class(const address &value) {
      if (value.is_v4()) {
        const auto bytes = value.to_v4().to_bytes();
        if (prefix_match(bytes, v4_127, 8)) return 0;  // PC
        if (prefix_match(bytes, v4_10, 8) || prefix_match(bytes, v4_172_16, 12) ||
            prefix_match(bytes, v4_192_168, 16) || prefix_match(bytes, v4_100_64, 10) ||
            prefix_match(bytes, v4_169_254, 16)) {
          return 1;  // LAN
        }
        return 2;
      }
      const auto bytes = value.to_v6().to_bytes();
      if (value.to_v6().is_loopback()) return 0;
      if (prefix_match(bytes, v6_unique_local, 7) || prefix_match(bytes, v6_link_local, 64)) return 1;
      return 2;
    }

    bool
    is_recent(const std::string &at, std::int64_t now) {
      const auto parsed = parse_utc_iso(at);
      if (!parsed) return false;
      if (*parsed > now + 300) return false;  // Tolerate small clock skew only.
      return now - *parsed <= EVIDENCE_WINDOW_SECONDS;
    }

    bool
    same_address(std::string_view left, std::string_view right) {
      if (left.empty() || right.empty()) return false;
      const auto first = parse_address(left);
      const auto second = parse_address(right);
      if (first && second) return *first == *second;
      return left == right;
    }

    bool
    has_code(const std::vector<std::string> &codes, std::string_view code) {
      return std::find(codes.begin(), codes.end(), code) != codes.end();
    }

    const mapping_t *
    find_mapping(const router_status_t &router, const std::string &proto, std::uint16_t port) {
      for (const auto &mapping : router.mappings) {
        if (mapping.port == port && mapping.proto == proto) return &mapping;
      }
      return nullptr;
    }

    std::vector<const port_status_t *>
    required_ports(const std::vector<port_status_t> &ports, bool web_ui_required) {
      std::vector<const port_status_t *> required;
      for (const auto &port : ports) {
        if (port.role == port_role::web && !web_ui_required) continue;
        required.push_back(&port);
      }
      return required;
    }

    bool
    is_wildcard(std::string_view value) {
      return value == "0.0.0.0" || value == "::" || value == "*";
    }

    std::string
    code(std::string_view prefix, const port_status_t &port) {
      return std::string(prefix) + port.proto + "/" + std::to_string(port.port);
    }

    std::mutex observation_mutex;
    std::vector<evidence_t> observations;
    constexpr std::size_t MAX_OBSERVATIONS = 16;
  }  // namespace

  ip_class_e
  classify_wan_ip(std::string_view value) {
    const auto parsed = parse_address(value);
    if (!parsed || is_unspecified(*parsed)) return ip_class_e::unknown;
    if (parsed->is_v4()) {
      const auto bytes = parsed->to_v4().to_bytes();
      if (prefix_match(bytes, v4_100_64, 10)) return ip_class_e::cgnat;
      if (is_public_v4(parsed->to_v4())) return ip_class_e::public_address;
      if (prefix_match(bytes, v4_multicast, 4) || prefix_match(bytes, v4_reserved, 4)) return ip_class_e::unknown;
      return ip_class_e::private_address;
    }
    if (parsed->to_v6().is_v4_mapped()) {
      return classify_wan_ip(boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, parsed->to_v6()).to_string());
    }
    if (is_public_v6(parsed->to_v6())) return ip_class_e::public_address;
    return ip_class_e::private_address;
  }

  bool
  is_mesh_address(std::string_view value) {
    const auto parsed = parse_address(value);
    if (!parsed) return false;
    if (parsed->is_v4()) {
      const auto bytes = parsed->to_v4().to_bytes();
      return prefix_match(bytes, v4_100_64, 10) || prefix_match(bytes, v4_10_144_144, 24);
    }
    return prefix_match(parsed->to_v6().to_bytes(), v6_mesh, 48);
  }

  mesh_kind_e
  classify_adapter(std::string_view name, std::string_view description, const std::vector<std::string> &addresses) {
    std::string haystack { name };
    haystack += ' ';
    haystack += description;
    std::transform(haystack.begin(), haystack.end(), haystack.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (haystack.find("tailscale") != std::string::npos) return mesh_kind_e::tailscale;
    if (haystack.find("netbird") != std::string::npos) return mesh_kind_e::netbird;
    if (haystack.find("zerotier") != std::string::npos || haystack.find("zero tier") != std::string::npos) return mesh_kind_e::zerotier;
    if (haystack.rfind("zt", 0) == 0) return mesh_kind_e::zerotier;
    if (haystack.find("easytier") != std::string::npos || haystack.find("easy tier") != std::string::npos) return mesh_kind_e::easytier;
    if (haystack.find("wireguard") != std::string::npos || haystack.rfind("wg", 0) == 0 || haystack.find(" wg") != std::string::npos) {
      return mesh_kind_e::wireguard;
    }
    // NetBird names its Linux interface wt<index>; the description is not
    // available through getifaddrs.
    if (haystack.rfind("wt", 0) == 0) return mesh_kind_e::netbird;
    for (const auto &address_value : addresses) {
      if (is_mesh_address(address_value)) return mesh_kind_e::other;
    }
    return mesh_kind_e::none;
  }

  std::string
  mask_address(std::string_view value) {
    const auto parsed = parse_address(value);
    if (!parsed) return {};
    if (parsed->is_v4()) {
      const auto bytes = parsed->to_v4().to_bytes();
      return std::to_string(bytes[0]) + '.' + std::to_string(bytes[1]) + '.' + std::to_string(bytes[2]) + ".0/24";
    }
    const auto bytes = parsed->to_v6().to_bytes();
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "%02x%02x:%02x%02x:%02x%02x::/64",
      bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]);
    return buffer;
  }

  bool
  origin_allows(std::string_view origin, std::string_view value) {
    const auto parsed = parse_address(value);
    if (!parsed) return false;
    const int allowed = origin == "wan" ? 2 : origin == "lan" ? 1 :
                                                                0;
    return origin_class(*parsed) <= allowed;
  }

  configured_address_t
  evaluate_configured_address(std::string_view configured_value, const std::vector<std::string> &resolved, std::string_view wan_ip) {
    // Address syntax: rfc1123-20261103 section 2.1, rfc1035-20261103 section 2.3.1.
    configured_address_t result;
    const auto text = public_address::normalize(configured_value);
    auto kind = public_address::classify(text);
    if (kind == public_address::kind_e::invalid) kind = public_address::kind_e::empty;
    result.kind = kind;
    if (kind == public_address::kind_e::empty) {
      result.reasons.push_back("unset");
      result.hint = "未设置公网地址时客户端使用自动发现的地址";
      return result;
    }
    result.value = text;

    const bool wan_public = classify_wan_ip(wan_ip) == ip_class_e::public_address;
    if (kind == public_address::kind_e::ip) {
      result.matches_wan = same_address(text, wan_ip);
      result.is_private = classify_wan_ip(text) != ip_class_e::public_address;
      if (result.is_private)
        result.reasons.push_back("address_private");
      else if (wan_public && !result.matches_wan)
        result.reasons.push_back("address_mismatch");
      return result;
    }

    for (const auto &entry : resolved) {
      const auto parsed = parse_address(entry);
      const auto canonical = parsed ? parsed->to_string() : std::string { entry };
      if (std::find(result.resolved.begin(), result.resolved.end(), canonical) != result.resolved.end()) continue;
      result.resolved.push_back(canonical);
    }
    if (result.resolved.empty()) {
      result.reasons.push_back("dns_failed");
      return result;
    }

    bool all_private = true;
    for (const auto &entry : result.resolved) {
      if (same_address(entry, wan_ip)) result.matches_wan = true;
      if (classify_wan_ip(entry) == ip_class_e::public_address) all_private = false;
    }
    result.is_private = all_private;
    if (all_private)
      result.reasons.push_back("address_private");
    else if (wan_public && !result.matches_wan)
      result.reasons.push_back("address_mismatch");
    return result;
  }

  bool
  binds_on_demand(const port_status_t &port) {
    // The control, video, audio and microphone sockets are opened by stream::start_broadcast when the
    // first session starts, so an idle host has no listener on them.
    if (port.proto != "udp") return false;
    return port.role == port_role::video || port.role == port_role::control || port.role == port_role::audio || port.role == port_role::microphone;
  }

  scope_e
  scope_of(std::string_view value) {
    const auto parsed = parse_address(value);
    if (!parsed || is_unspecified(*parsed)) return scope_e::unknown;
    if (parsed->is_v4() ? (parsed->to_v4().is_multicast() || prefix_match(parsed->to_v4().to_bytes(), v4_reserved, 4)) : parsed->to_v6().is_multicast()) return scope_e::unknown;
    if (parsed->is_loopback()) return scope_e::loopback;
    if (is_mesh_address(value)) return scope_e::mesh;
    return classify_wan_ip(value) == ip_class_e::public_address ? scope_e::public_address : scope_e::lan;
  }

  scope_e
  scope_of_all(const std::vector<std::string> &addresses) {
    bool has_mesh = false;
    bool has_lan = false;
    bool has_loopback = false;
    for (const auto &entry : addresses) {
      switch (scope_of(entry)) {
        case scope_e::public_address:
          return scope_e::public_address;
        case scope_e::mesh:
          has_mesh = true;
          break;
        case scope_e::lan:
          has_lan = true;
          break;
        case scope_e::loopback:
          has_loopback = true;
          break;
        case scope_e::unknown:
          break;
      }
    }
    if (has_mesh) return scope_e::mesh;
    if (has_lan) return scope_e::lan;
    if (has_loopback) return scope_e::loopback;
    return scope_e::unknown;
  }

  namespace {
    constexpr std::size_t MAX_ADDRESSES_PER_ENDPOINT = 4;

    // How much a failure says: a certificate that is not ours means somebody answered, a refusal that a
    // host answered, a timeout that nothing did.
    int
    error_rank(const std::string &error) {
      if (error == "tls_failed") return 4;
      if (error == "refused") return 3;
      if (error == "timeout") return 2;
      if (error == "unreachable") return 1;
      return 0;
    }

    endpoint_result_t
    probe_one_endpoint(const public_address::endpoint_t &endpoint, std::uint16_t default_http_port, const std::string &wan_ip, const endpoint_prober_t &prober) {
      endpoint_result_t result;
      result.input = public_address::format_endpoint(endpoint);
      result.host = endpoint.host;
      result.kind = endpoint.kind;
      result.port_explicit = endpoint.port != 0;
      result.port = public_address::http_port_of(endpoint, default_http_port);
      result.https_port = public_address::https_port_of(result.port);

      std::vector<std::string> raw;
      if (endpoint.kind == public_address::kind_e::ip) {
        raw.push_back(endpoint.host);
      }
      else if (prober.resolve) {
        raw = prober.resolve(endpoint.host);
      }
      for (const auto &entry : raw) {
        const auto parsed = parse_address(entry);
        const auto canonical = parsed ? parsed->to_string() : entry;
        if (std::find(result.resolved.begin(), result.resolved.end(), canonical) == result.resolved.end()) result.resolved.push_back(canonical);
      }
      if (result.resolved.empty()) {
        result.error = "dns_failed";
        return result;
      }

      result.scope = scope_of_all(result.resolved);
      const auto wan = parse_address(wan_ip);
      if (result.scope == scope_e::public_address && wan && classify_wan_ip(wan_ip) == ip_class_e::public_address) {
        // Only addresses of the router's own family can be compared with its outside address.
        bool comparable = false;
        for (const auto &entry : result.resolved) {
          const auto parsed = parse_address(entry);
          if (!parsed || parsed->is_v4() != wan->is_v4() || scope_of(entry) != scope_e::public_address) continue;
          comparable = true;
          if (*parsed == *wan) result.matches_wan = true;
        }
        if (comparable && !result.matches_wan) result.reasons.push_back("address_mismatch");
      }
      if (result.scope == scope_e::lan) result.reasons.push_back("scope_lan");
      if (result.scope == scope_e::mesh) result.reasons.push_back("scope_mesh");
      if (result.scope == scope_e::loopback) result.reasons.push_back("scope_loopback");

      std::vector<std::string> targets;
      for (const auto &entry : result.resolved) {
        if (scope_of(entry) == scope_e::unknown) continue;
        if (targets.size() < MAX_ADDRESSES_PER_ENDPOINT) targets.push_back(entry);
      }
      if (targets.empty() || result.https_port == 0 || !prober.handshake) {
        result.error = "not_tried";
        return result;
      }

      std::vector<std::future<handshake_t>> jobs;
      for (const auto &target : targets) {
        jobs.push_back(std::async(std::launch::async, [&prober, target, port = result.https_port]() { return prober.handshake(target, port); }));
      }
      for (std::size_t index = 0; index < targets.size(); ++index) {
        endpoint_attempt_t attempt;
        attempt.address = targets[index];
        attempt.port = result.https_port;
        attempt.handshake = jobs[index].get();
        result.attempts.push_back(std::move(attempt));
      }

      const endpoint_attempt_t *decisive = nullptr;
      bool answered_by_other = false;
      std::string worst_error;
      for (const auto &attempt : result.attempts) {
        if (attempt.handshake.connected && attempt.handshake.identity_match) {
          if (!decisive || !result.reached) decisive = &attempt;
          result.reached = true;
        }
        else if (attempt.handshake.connected) {
          answered_by_other = true;
        }
        else if (error_rank(attempt.handshake.error) >= error_rank(worst_error)) {
          worst_error = attempt.handshake.error.empty() ? "failed" : attempt.handshake.error;
        }
      }
      if (!decisive) decisive = &result.attempts.front();
      result.tested_address = decisive->address;
      result.latency_ms = decisive->handshake.latency_ms;
      if (!result.reached) result.error = answered_by_other ? "identity_mismatch" : worst_error.empty() ? "failed" : worst_error;
      return result;
    }
  }  // namespace

  std::vector<endpoint_result_t>
  probe_endpoints(const std::vector<public_address::endpoint_t> &endpoints, std::uint16_t default_http_port,
    std::string_view wan_ip, const endpoint_prober_t &prober) {
    const std::string wan { wan_ip };
    std::vector<std::future<endpoint_result_t>> jobs;
    const auto count = std::min(endpoints.size(), public_address::MAX_ENDPOINTS);
    for (std::size_t index = 0; index < count; ++index) {
      jobs.push_back(std::async(std::launch::async, [&, index]() { return probe_one_endpoint(endpoints[index], default_http_port, wan, prober); }));
    }
    std::vector<endpoint_result_t> results;
    for (auto &job : jobs) results.push_back(job.get());
    return results;
  }

  public_result_t
  evaluate_public(const public_inputs_t &inputs, std::int64_t now) {
    public_result_t result;
    result.host_public_ip = inputs.host_public_ip;
    result.router = inputs.router;
    result.hairpin = inputs.hairpin;
    result.configured = inputs.configured;
    result.endpoints = inputs.endpoints;
    // No public address is configured: the host cannot prove the public path
    // either way, so the missing setting is a hint, not a failure. A configured
    // address (external_ip or external_endpoints) keeps the normal verdict.
    const bool no_public_address =
      inputs.configured.kind == public_address::kind_e::empty && inputs.endpoints.empty();
    if (no_public_address) {
      result.reasons.push_back("unset");
    }

    for (const auto &entry : inputs.evidence) {
      if (entry.kind == "client-connected-from-public" && is_recent(entry.at, now)) {
        result.evidence.push_back(entry);
      }
    }

    const auto required = required_ports(inputs.ports, inputs.web_ui_exposed_to_wan);
    bool all_listening = true;
    bool any_blocked = false;
    bool firewall_unknown = false;
    bool mappings_complete = true;
    for (const auto *port : required) {
      if (!port->listening && !binds_on_demand(*port)) {
        all_listening = false;
        result.reasons.push_back(code("not_listening:", *port));
      }
      if (port->firewall == firewall_e::blocked) {
        any_blocked = true;
        result.reasons.push_back(code("firewall_blocked:", *port));
      }
      else if (port->firewall == firewall_e::unknown) {
        firewall_unknown = true;
      }
    }

    bool evidence_public = !result.evidence.empty();

    if (evidence_public) {
      result.verdict = verdict_e::verified;
      result.reasons.insert(result.reasons.begin(), "session_evidence");
      return result;
    }
    if (!all_listening) {
      result.verdict = verdict_e::unreachable;
      return result;
    }
    if (any_blocked) {
      result.verdict = verdict_e::unreachable;
      return result;
    }

    // The configured public address is what the host tells clients to use.
    // A private value cannot work outside the LAN; a value that does not match
    // the router's WAN address leaves the outcome unconfirmed unless the
    // hairpin handshake already proved that the configured address reaches us.
    if (!inputs.endpoints.empty()) {
      bool any_public = false;
      bool all_public_mismatch = true;
      bool any_mesh = false;
      bool any_unresolved = false;
      for (const auto &endpoint : inputs.endpoints) {
        if (endpoint.scope == scope_e::public_address) {
          any_public = true;
          if (endpoint.reached) {
            result.reasons.push_back("hairpin_verified");
            result.verdict = verdict_e::verified;
            return result;
          }
          if (!has_code(endpoint.reasons, "address_mismatch")) all_public_mismatch = false;
        }
        else if (endpoint.scope == scope_e::mesh) {
          any_mesh = true;
        }
        else if (endpoint.error == "dns_failed") {
          any_unresolved = true;
        }
      }
      if (!any_public) {
        // Nothing the internet could use: private and loopback addresses cannot be reached from outside.
        if (any_unresolved) {
          result.reasons.push_back("dns_failed");
          result.verdict = verdict_e::unknown;
          return result;
        }
        if (!any_mesh) {
          result.reasons.push_back("address_private");
          result.verdict = verdict_e::unreachable;
          return result;
        }
      }
      else if (all_public_mismatch) {
        result.reasons.push_back("address_mismatch");
        result.verdict = verdict_e::unknown;
        return result;
      }
    }
    else if (inputs.configured.kind != public_address::kind_e::empty) {
      const auto &configured = inputs.configured;
      if (configured.is_private) {
        result.reasons.push_back("address_private");
        result.verdict = verdict_e::unreachable;
        return result;
      }
      if (has_code(configured.reasons, "dns_failed")) {
        result.reasons.push_back("dns_failed");
        result.verdict = verdict_e::unknown;
        return result;
      }
      if (has_code(configured.reasons, "address_mismatch")) {
        if (inputs.hairpin.tried && inputs.hairpin.ok && inputs.hairpin.identity_match) {
          result.reasons.push_back("hairpin_verified");
          result.verdict = verdict_e::verified;
          return result;
        }
        result.reasons.push_back("address_mismatch");
        result.verdict = verdict_e::unknown;
        return result;
      }
      if (inputs.hairpin.tried && inputs.hairpin.ok && inputs.hairpin.identity_match) {
        result.reasons.push_back("hairpin_verified");
        result.verdict = verdict_e::verified;
        return result;
      }
    }

    if (!inputs.host_public_ip.empty()) {
      result.reasons.insert(result.reasons.begin(), "host_public_ip");
      if (firewall_unknown) {
        result.reasons.push_back("firewall_unknown");
        result.verdict = verdict_e::likely;
      }
      else {
        result.verdict = verdict_e::verified;
      }
      return result;
    }

    switch (inputs.router.wan_ip_class) {
      case ip_class_e::cgnat:
        result.reasons.push_back("cgnat");
        result.verdict = no_public_address ? verdict_e::unknown : verdict_e::unreachable;
        return result;
      case ip_class_e::private_address:
        result.reasons.push_back("private_wan");
        result.verdict = no_public_address ? verdict_e::unknown : verdict_e::unreachable;
        return result;
      case ip_class_e::unknown:
        result.reasons.push_back(inputs.router.protocol == "none" ? "no_igd" : "no_wan_ip");
        result.verdict = verdict_e::unknown;
        return result;
      case ip_class_e::public_address:
        break;
    }

    for (const auto *port : required) {
      const auto *mapping = find_mapping(inputs.router, port->proto, port->port);
      if (!mapping || !mapping->present) {
        mappings_complete = false;
        result.reasons.push_back(code("mapping_missing:", *port));
      }
    }
    if (!mappings_complete) {
      result.verdict = verdict_e::unknown;
      return result;
    }

    if (inputs.hairpin.tried && inputs.hairpin.ok && inputs.hairpin.identity_match) {
      result.reasons.push_back("mappings_complete");
      result.reasons.push_back("hairpin_verified");
      result.verdict = verdict_e::verified;
      return result;
    }

    result.reasons.push_back("mappings_complete");
    result.reasons.push_back(inputs.hairpin.tried ? "hairpin_unsupported" : "hairpin_not_tried");
    if (firewall_unknown) {
      result.reasons.push_back("firewall_unknown");
      result.verdict = verdict_e::unknown;
    }
    else {
      result.verdict = verdict_e::likely;
    }
    return result;
  }

  mesh_result_t
  evaluate_mesh(const mesh_inputs_t &inputs, std::int64_t now) {
    mesh_result_t result;
    result.adapters = inputs.adapters;
    result.web_ui_origin_allows_mesh = inputs.origin_allows_mesh;

    for (const auto &entry : inputs.evidence) {
      if (entry.kind == "client-connected-from-mesh" && is_recent(entry.at, now)) {
        result.evidence.push_back(entry);
      }
    }

    if (inputs.adapters.empty()) {
      result.verdict = mesh_verdict_e::none;
      result.reasons.push_back("no_mesh_adapter");
      return result;
    }

    bool any_up = false;
    std::vector<std::string> mesh_addresses;
    for (const auto &adapter : inputs.adapters) {
      any_up = any_up || adapter.up;
      mesh_addresses.insert(mesh_addresses.end(), adapter.addresses.begin(), adapter.addresses.end());
    }

    const auto required = required_ports(inputs.ports, true);
    bool all_listening = true;
    bool any_blocked = false;
    for (const auto *port : required) {
      if (!port->listening) {
        all_listening = false;
        result.reasons.push_back(code("not_listening_on_mesh:", *port));
        continue;
      }
      bool bound_to_mesh = false;
      for (const auto &bound : port->bound) {
        if (is_wildcard(bound)) {
          bound_to_mesh = true;
          break;
        }
        if (std::find(mesh_addresses.begin(), mesh_addresses.end(), bound) != mesh_addresses.end()) {
          bound_to_mesh = true;
          break;
        }
      }
      if (!bound_to_mesh) {
        all_listening = false;
        result.reasons.push_back(code("not_listening_on_mesh:", *port));
      }
      if (port->firewall == firewall_e::blocked) {
        any_blocked = true;
        result.reasons.push_back(code("firewall_blocked:", *port));
      }
    }
    result.listening_on_mesh = all_listening;

    if (!inputs.origin_allows_mesh) result.reasons.push_back("origin_blocks_mesh");

    if (!result.evidence.empty()) {
      result.verdict = mesh_verdict_e::verified;
      result.reasons.insert(result.reasons.begin(), "session_evidence");
      return result;
    }
    if (!any_up) {
      result.verdict = mesh_verdict_e::unknown;
      result.reasons.push_back("mesh_adapter_down");
      return result;
    }
    if (!all_listening || any_blocked) {
      result.verdict = mesh_verdict_e::unknown;
      return result;
    }
    result.verdict = mesh_verdict_e::likely;
    result.reasons.push_back("mesh_adapter_up");
    return result;
  }

  nlohmann::json
  to_json(const snapshot_t &snapshot) {
    nlohmann::json output;
    output["version"] = snapshot.version;
    output["checked_at"] = snapshot.checked_at;
    output["ports"] = nlohmann::json::array();
    for (const auto &port : snapshot.ports) {
      output["ports"].push_back({
        { "proto", port.proto },
        { "port", port.port },
        { "role", port.role },
        { "listening", port.listening },
        { "bound", port.bound },
        { "firewall", to_string(port.firewall) },
        { "on_demand", binds_on_demand(port) },
      });
    }

    const auto &public_result = snapshot.public_reachability;
    nlohmann::json public_json {
      { "verdict", to_string(public_result.verdict) },
      { "reasons", public_result.reasons },
      { "host_public_ip", public_result.host_public_ip.empty() ? nlohmann::json(nullptr) : nlohmann::json(public_result.host_public_ip) },
      { "router", {
                    { "protocol", public_result.router.protocol },
                    { "wan_ip", public_result.router.wan_ip.empty() ? nlohmann::json(nullptr) : nlohmann::json(public_result.router.wan_ip) },
                    { "wan_ip_class", to_string(public_result.router.wan_ip_class) },
                    { "mappings", nlohmann::json::array() },
                  } },
      { "hairpin", {
                     { "tried", public_result.hairpin.tried },
                     { "ok", public_result.hairpin.ok },
                     { "port", public_result.hairpin.port },
                     { "identity_match", public_result.hairpin.identity_match },
                     { "target", public_result.hairpin.target.empty() ? nlohmann::json(nullptr) : nlohmann::json(public_result.hairpin.target) },
                   } },
      { "configured_address", {
                                { "value", public_result.configured.value.empty() ? nlohmann::json(nullptr) : nlohmann::json(public_result.configured.value) },
                                { "kind", public_address::to_string(public_result.configured.kind) },
                                { "resolved", public_result.configured.resolved },
                                { "matches_wan", public_result.configured.matches_wan },
                                { "reasons", public_result.configured.reasons },
                                { "hint", public_result.configured.hint },
                              } },
      { "endpoints", nlohmann::json::array() },
      { "evidence", nlohmann::json::array() },
    };
    for (const auto &endpoint : public_result.endpoints) {
      nlohmann::json attempts = nlohmann::json::array();
      for (const auto &attempt : endpoint.attempts) {
        attempts.push_back({
          { "address", attempt.address },
          { "port", attempt.port },
          { "connected", attempt.handshake.connected },
          { "identity_match", attempt.handshake.identity_match },
          { "error", attempt.handshake.error.empty() ? nlohmann::json(nullptr) : nlohmann::json(attempt.handshake.error) },
          { "latency_ms", attempt.handshake.latency_ms },
        });
      }
      nlohmann::json tested = nullptr;
      if (!endpoint.tested_address.empty()) {
        tested = { { "address", endpoint.tested_address }, { "port", endpoint.https_port }, { "latency_ms", endpoint.latency_ms } };
      }
      public_json["endpoints"].push_back({
        { "input", endpoint.input },
        { "host", endpoint.host },
        { "kind", public_address::to_string(endpoint.kind) },
        { "port", endpoint.port },
        { "port_explicit", endpoint.port_explicit },
        { "https_port", endpoint.https_port },
        { "resolved", endpoint.resolved },
        { "scope", to_string(endpoint.scope) },
        { "matches_wan", endpoint.matches_wan },
        { "state", endpoint.reached ? "reached" : "failed" },
        { "error", endpoint.error.empty() ? nlohmann::json(nullptr) : nlohmann::json(endpoint.error) },
        { "tested", std::move(tested) },
        { "attempts", std::move(attempts) },
        { "reasons", endpoint.reasons },
      });
    }
    for (const auto &mapping : public_result.router.mappings) {
      public_json["router"]["mappings"].push_back({
        { "proto", mapping.proto },
        { "port", mapping.port },
        { "present", mapping.present },
        { "internal", mapping.internal },
      });
    }
    for (const auto &entry : public_result.evidence) {
      public_json["evidence"].push_back({ { "kind", entry.kind }, { "address", entry.address }, { "at", entry.at } });
    }
    output["public"] = std::move(public_json);

    const auto &mesh_result = snapshot.mesh;
    nlohmann::json mesh_json {
      { "adapters", nlohmann::json::array() },
      { "verdict", to_string(mesh_result.verdict) },
      { "reasons", mesh_result.reasons },
      { "listening_on_mesh", mesh_result.listening_on_mesh },
      { "web_ui_origin_allows_mesh", mesh_result.web_ui_origin_allows_mesh },
      { "evidence", nlohmann::json::array() },
    };
    for (const auto &adapter : mesh_result.adapters) {
      mesh_json["adapters"].push_back({
        { "kind", to_string(adapter.kind) },
        { "name", adapter.name },
        { "addresses", adapter.addresses },
        { "up", adapter.up },
      });
    }
    for (const auto &entry : mesh_result.evidence) {
      mesh_json["evidence"].push_back({ { "kind", entry.kind }, { "address", entry.address }, { "at", entry.at } });
    }
    output["mesh"] = std::move(mesh_json);
    return output;
  }

  std::optional<std::string>
  parse_igd_external_ip(std::string_view raw) {
    const auto value = std::string(raw);
    const auto parsed = parse_address(value);
    if (!parsed || is_unspecified(*parsed)) return std::nullopt;
    return parsed->to_string();
  }

  std::optional<mapping_t>
  parse_igd_mapping(int error_code, std::string_view internal_client, std::string_view lease_duration,
    std::string_view proto, std::uint16_t port) {
    // 714 is NoSuchEntryInArray: the gateway answered and the mapping is absent.
    if (error_code == 714) {
      return mapping_t { std::string(proto), port, false, {} };
    }
    if (error_code != 0) return std::nullopt;
    const auto internal = std::string(internal_client);
    if (internal.empty()) return mapping_t { std::string(proto), port, false, {} };
    const auto parsed = parse_address(internal);
    if (!parsed) return std::nullopt;
    static_cast<void>(lease_duration);
    return mapping_t { std::string(proto), port, true, parsed->to_string() };
  }

  std::string
  test_endpoints_option(std::string_view raw) {
    // The query value is the caller's: bound it before parsing.
    if (raw.size() > 1024) raw = raw.substr(0, 1024);
    return public_address::join_endpoints(public_address::parse_endpoint_list(raw).endpoints);
  }

  std::string
  format_utc_iso(std::int64_t epoch_seconds) {
    auto value = static_cast<std::time_t>(epoch_seconds);
    std::tm broken_down {};
#ifdef _WIN32
    gmtime_s(&broken_down, &value);
#else
    gmtime_r(&value, &broken_down);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &broken_down);
    return buffer;
  }

  std::int64_t
  now_epoch_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch())
      .count();
  }

  std::optional<std::int64_t>
  parse_utc_iso(std::string_view value) {
    if (value.size() != 20 || value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
        value[13] != ':' || value[16] != ':' || value[19] != 'Z') {
      return std::nullopt;
    }
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    const auto text = std::string(value);
    if (std::sscanf(text.c_str(), "%4d-%2d-%2dT%2d:%2d:%2dZ", &year, &month, &day, &hour, &minute, &second) != 6) {
      return std::nullopt;
    }
    if (year < 1970 || month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60) {
      return std::nullopt;
    }
    std::tm broken_down {};
    broken_down.tm_year = year - 1900;
    broken_down.tm_mon = month - 1;
    broken_down.tm_mday = day;
    broken_down.tm_hour = hour;
    broken_down.tm_min = minute;
    broken_down.tm_sec = second;
#ifdef _WIN32
    const auto epoch = _mkgmtime(&broken_down);
#else
    const auto epoch = timegm(&broken_down);
#endif
    if (epoch < 0) return std::nullopt;
    return static_cast<std::int64_t>(epoch);
  }

  void
  observe_client(std::string_view value, std::int64_t epoch_seconds) {
    if (value.empty()) return;
    const auto masked = mask_address(value);
    if (masked.empty()) return;
    const bool mesh = is_mesh_address(value);
    if (!mesh && classify_wan_ip(value) != ip_class_e::public_address) return;

    const evidence_t entry {
      mesh ? "client-connected-from-mesh" : "client-connected-from-public",
      masked,
      format_utc_iso(epoch_seconds),
    };
    std::lock_guard lock { observation_mutex };
    observations.erase(
      std::remove_if(observations.begin(), observations.end(), [&](const evidence_t &item) {
        return item.kind == entry.kind && item.address == entry.address;
      }),
      observations.end());
    observations.insert(observations.begin(), entry);
    if (observations.size() > MAX_OBSERVATIONS) observations.resize(MAX_OBSERVATIONS);
  }

  std::vector<evidence_t>
  observed_evidence(std::int64_t now) {
    std::lock_guard lock { observation_mutex };
    std::vector<evidence_t> result;
    for (const auto &entry : observations) {
      if (is_recent(entry.at, now)) result.push_back(entry);
    }
    return result;
  }

  std::string_view
  to_string(verdict_e value) {
    switch (value) {
      case verdict_e::verified:
        return "verified";
      case verdict_e::likely:
        return "likely";
      case verdict_e::unknown:
        return "unknown";
      case verdict_e::unreachable:
        return "unreachable";
    }
    return "unknown";
  }

  std::string_view
  to_string(mesh_verdict_e value) {
    switch (value) {
      case mesh_verdict_e::verified:
        return "verified";
      case mesh_verdict_e::likely:
        return "likely";
      case mesh_verdict_e::unknown:
        return "unknown";
      case mesh_verdict_e::none:
        return "none";
    }
    return "none";
  }

  std::string_view
  to_string(ip_class_e value) {
    switch (value) {
      case ip_class_e::public_address:
        return "public";
      case ip_class_e::private_address:
        return "private";
      case ip_class_e::cgnat:
        return "cgnat";
      case ip_class_e::unknown:
        return "unknown";
    }
    return "unknown";
  }

  std::string_view
  to_string(firewall_e value) {
    switch (value) {
      case firewall_e::allowed:
        return "allowed";
      case firewall_e::blocked:
        return "blocked";
      case firewall_e::unknown:
        return "unknown";
    }
    return "unknown";
  }

  std::string_view
  to_string(scope_e value) {
    switch (value) {
      case scope_e::public_address:
        return "public";
      case scope_e::lan:
        return "lan";
      case scope_e::mesh:
        return "mesh";
      case scope_e::loopback:
        return "loopback";
      case scope_e::unknown:
        return "unknown";
    }
    return "unknown";
  }

  std::string_view
  to_string(mesh_kind_e value) {
    switch (value) {
      case mesh_kind_e::netbird:
        return "netbird";
      case mesh_kind_e::tailscale:
        return "tailscale";
      case mesh_kind_e::zerotier:
        return "zerotier";
      case mesh_kind_e::easytier:
        return "easytier";
      case mesh_kind_e::wireguard:
        return "wireguard";
      case mesh_kind_e::other:
        return "other";
      case mesh_kind_e::none:
        return "none";
    }
    return "none";
  }
}  // namespace reachability
