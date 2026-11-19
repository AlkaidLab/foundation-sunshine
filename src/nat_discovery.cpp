/**
 * @file src/nat_discovery.cpp
 * @brief STUN message handling, the RFC 5780 mapping decision, address classes and the overview contract.
 *
 * Sources: STUN, RFC 8489 sections 5, 6 and 14 (rfc8489-20261104); NAT behaviour discovery,
 * RFC 5780 sections 4.3 and 7.4 (rfc5780-20261104); behaviour terms, RFC 4787 section 4.1
 * (rfc4787-20261104); temporary addresses, RFC 8981 (rfc8981-20261104); address selection,
 * RFC 6724 (rfc6724-20261104).
 */
#include "nat_discovery.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>

#include <boost/asio/ip/address.hpp>

#include <alkaidlab/network/stun_codec.h>

#include "public_address.h"

using namespace std::literals;

namespace nat_discovery {
  namespace {
    namespace ip = boost::asio::ip;

    std::optional<ip::address>
    parse_ip(std::string_view text) {
      std::string value { text };
      const auto percent = value.find('%');
      if (percent != std::string::npos) value.erase(percent);
      boost::system::error_code error;
      auto parsed = ip::make_address(value, error);
      if (error) return std::nullopt;
      if (parsed.is_v6() && parsed.to_v6().is_v4_mapped()) {
        return ip::address { ip::make_address_v4(ip::v4_mapped, parsed.to_v6()) };
      }
      return parsed;
    }

    std::optional<transport_address_t>
    from_codec(const AlkStunMappedEndpoint &endpoint) {
      if (endpoint.address_family == ALK_STUN_ADDRESS_IPV4) {
        ip::address_v4::bytes_type bytes;
        std::memcpy(bytes.data(), endpoint.address, bytes.size());
        return transport_address_t { ip::address_v4(bytes).to_string(), endpoint.port };
      }
      if (endpoint.address_family == ALK_STUN_ADDRESS_IPV6) {
        ip::address_v6::bytes_type bytes;
        std::memcpy(bytes.data(), endpoint.address, bytes.size());
        return transport_address_t { ip::address_v6(bytes).to_string(), endpoint.port };
      }
      return std::nullopt;
    }

    bool
    in_v4(const ip::address_v4 &value, std::uint32_t network, int bits) {
      const auto mask = bits == 0 ? 0u : ~std::uint32_t { 0 } << (32 - bits);
      return (value.to_uint() & mask) == (network & mask);
    }

    bool
    has(const std::vector<std::string> &values, std::string_view value) {
      return std::find(values.begin(), values.end(), value) != values.end();
    }

    nlohmann::json
    transport_json(const std::optional<transport_address_t> &value) {
      if (!value) return nullptr;
      return { { "address", value->address }, { "port", value->port } };
    }

    /// The TCP ports a client needs from outside; the UDP stream ports open with the first session.
    std::vector<const reachability::port_status_t *>
    client_tcp_ports(const reachability::snapshot_t &snapshot) {
      std::vector<const reachability::port_status_t *> result;
      for (const auto &port : snapshot.ports) {
        if (port.proto != "tcp") continue;
        if (port.role == reachability::port_role::https || port.role == reachability::port_role::http || port.role == reachability::port_role::rtsp) {
          result.push_back(&port);
        }
      }
      return result;
    }
  }  // namespace

  std::vector<server_t>
  default_servers(region_e region) {
    // Verified 2026-11-04 from Shanghai (China Telecom); see docs/api/network-overview.md for the table.
    // Google does not answer on the direct route from mainland China, so the China list does not ask it.
    if (region == region_e::china) {
      return {
        { "stun.miwifi.com", 3478 },
        { "stun.chat.bilibili.com", 3478 },
        { "stun.douyucdn.cn", 18000 },
        { "stun.hitv.com", 3478 },
        { "stun.cloudflare.com", 3478 },
      };
    }
    return {
      { "stun.cloudflare.com", 3478 },
      { "stun.l.google.com", 19302 },
      { "stun.miwifi.com", 3478 },
      { "stun.chat.bilibili.com", 3478 },
      { "stun.douyucdn.cn", 18000 },
      { "stun.hitv.com", 3478 },
    };
  }

  region_e
  region_of(std::string_view locale_text, std::string_view time_zone) {
    std::string locale { locale_text };
    std::transform(locale.begin(), locale.end(), locale.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::replace(locale.begin(), locale.end(), '-', '_');
    if (locale == "zh" || locale == "zh_cn" || locale == "zh_hans" || locale == "zh_hans_cn") return region_e::china;
    std::string zone { time_zone };
    const auto slash = zone.find("zoneinfo/");
    if (slash != std::string::npos) zone = zone.substr(slash + 9);
    for (const auto name : { "Asia/Shanghai", "Asia/Chongqing", "Asia/Chungking", "Asia/Harbin", "Asia/Urumqi", "Asia/Kashgar", "PRC", "China Standard Time" }) {
      if (zone == name) return region_e::china;
    }
    return region_e::global;
  }

  server_list_t
  parse_server_list(std::string_view text, region_e region) {
    server_list_t result;
    auto trimmed = public_address::trim(text);
    std::string lowered = trimmed;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lowered.empty()) {
      result.servers = default_servers(region);
      return result;
    }
    if (lowered == "none" || lowered == "off") {
      result.disabled = true;
      return result;
    }
    result.configured = true;
    const auto list = public_address::parse_endpoint_list(trimmed);
    result.rejected = list.rejected;
    result.truncated = list.truncated;
    for (const auto &endpoint : list.endpoints) {
      if (result.servers.size() == MAX_SERVERS) {
        result.truncated = true;
        break;
      }
      result.servers.push_back({ endpoint.host, endpoint.port != 0 ? endpoint.port : DEFAULT_PORT });
    }
    return result;
  }

  std::string
  join_servers(const std::vector<server_t> &servers) {
    std::vector<public_address::endpoint_t> endpoints;
    for (const auto &server : servers) {
      endpoints.push_back({ server.host, server.port, public_address::classify(server.host) });
    }
    return public_address::join_endpoints(endpoints);
  }

  std::string
  format_transport(const transport_address_t &value) {
    if (value.address.find(':') != std::string::npos) return "["s + value.address + "]:" + std::to_string(value.port);
    return value.address + ":" + std::to_string(value.port);
  }

  std::vector<std::uint8_t>
  build_binding_request(const transaction_id_t &transaction_id) {
    std::vector<std::uint8_t> storage(ALK_STUN_HEADER_BYTES);
    AlkStunMessageBuilder builder;
    if (!alk_stun_builder_init(&builder, storage.data(), storage.size(), ALK_STUN_BINDING_REQUEST, transaction_id.data())) {
      return {};
    }
    storage.resize(builder.length);
    return storage;
  }

  std::optional<binding_response_t>
  parse_binding_response(const std::uint8_t *data, std::size_t length, const transaction_id_t &transaction_id) {
    // RFC 8489 section 6.3: a response is matched by its transaction ID; class success (0x0101) or error (0x0111).
    constexpr std::uint16_t binding_error_response = 0x0111;
    if (!data) return std::nullopt;
    binding_response_t response;
    if (alk_stun_message_matches(data, length, ALK_STUN_BINDING_SUCCESS_RESPONSE, transaction_id.data())) {
      response.success = true;
      AlkStunMappedEndpoint endpoint;
      alk_stun_mapped_endpoint_init(&endpoint);
      if (alk_stun_decode_mapped_endpoint(data, length, transaction_id.data(), &endpoint)) {
        response.mapped = from_codec(endpoint);
      }
      alk_stun_mapped_endpoint_init(&endpoint);
      if (alk_stun_decode_other_address(data, length, transaction_id.data(), &endpoint)) {
        response.other = from_codec(endpoint);
      }
      alk_stun_mapped_endpoint_init(&endpoint);
      if (alk_stun_decode_response_origin(data, length, transaction_id.data(), &endpoint)) {
        response.origin = from_codec(endpoint);
      }
      return response;
    }
    if (alk_stun_message_matches(data, length, binding_error_response, transaction_id.data())) {
      std::uint16_t code = 0;
      alk_stun_decode_error_code(data, length, transaction_id.data(), &code);
      response.error_code = code;
      return response;
    }
    return std::nullopt;
  }

  mapping_result_t
  classify_mapping(const mapping_inputs_t &inputs) {
    mapping_result_t result;
    result.method = inputs.method;
    if (!inputs.test1) {
      result.reasons.push_back("no_answer");
      return result;
    }

    // Test I: the reflexive address equals the socket's own address and port, so there is no NAT and the
    // effective mapping is endpoint-independent (RFC 5780 section 4.3, first paragraph).
    const auto &test1 = *inputs.test1;
    bool own_address = has(inputs.local_addresses, test1.address);
    if (inputs.local && inputs.local->address == test1.address) own_address = true;
    if (own_address && inputs.local && inputs.local->port == test1.port) {
      result.nat_present = false;
      result.mapping = mapping_e::endpoint_independent;
      result.reasons.push_back("not_natted");
      return result;
    }
    result.nat_present = true;
    if (own_address) {
      // The host's own address with another port: a host firewall or a port-translating middlebox.
      result.reasons.push_back("port_translated");
    }

    if (inputs.method == method_e::none) {
      result.reasons.push_back("single_server");
      return result;
    }
    if (!inputs.test2) {
      result.reasons.push_back("test2_no_answer");
      return result;
    }
    if (inputs.test2->address != test1.address) result.mapped_address_varies = true;
    if (*inputs.test2 == test1) {
      result.mapping = mapping_e::endpoint_independent;
      return result;
    }

    if (inputs.method == method_e::two_servers || !inputs.test3_possible) {
      // A second server at another address shows the dependence; without an alternate port at that same
      // address, test III cannot separate address-dependent from address-and-port-dependent.
      result.mapping = mapping_e::endpoint_dependent;
      result.reasons.push_back("test3_not_possible");
      return result;
    }
    if (!inputs.test3) {
      result.mapping = mapping_e::endpoint_dependent;
      result.reasons.push_back("test3_no_answer");
      return result;
    }
    if (inputs.test3->address != inputs.test2->address) result.mapped_address_varies = true;
    result.mapping = *inputs.test3 == *inputs.test2 ? mapping_e::address_dependent : mapping_e::address_and_port_dependent;
    return result;
  }

  address_scope_e
  classify_address(std::string_view text) {
    const auto parsed = parse_ip(text);
    if (!parsed) return address_scope_e::other;
    if (parsed->is_loopback()) return address_scope_e::loopback;
    if (parsed->is_unspecified() || parsed->is_multicast()) return address_scope_e::other;
    if (parsed->is_v4()) {
      const auto v4 = parsed->to_v4();
      if (in_v4(v4, 0x0A000000, 8) || in_v4(v4, 0xAC100000, 12) || in_v4(v4, 0xC0A80000, 16)) return address_scope_e::private_v4;
      if (in_v4(v4, 0x64400000, 10)) return address_scope_e::cgnat;
      if (in_v4(v4, 0xA9FE0000, 16)) return address_scope_e::link_local;
      // 0/8, 192.0.0/24, documentation ranges, 198.18/15 benchmarking, 240/4 reserved (RFC 6890).
      if (in_v4(v4, 0x00000000, 8) || in_v4(v4, 0xC0000000, 24) || in_v4(v4, 0xC0000200, 24) || in_v4(v4, 0xC6336400, 24) ||
          in_v4(v4, 0xCB007100, 24) || in_v4(v4, 0xC6120000, 15) || in_v4(v4, 0xF0000000, 4)) {
        return address_scope_e::other;
      }
      return address_scope_e::global;
    }
    const auto v6 = parsed->to_v6();
    const auto bytes = v6.to_bytes();
    if (v6.is_link_local()) return address_scope_e::link_local;
    if ((bytes[0] & 0xFE) == 0xFC) return address_scope_e::ula;
    // Global unicast is 2000::/3 (RFC 4291 section 2.4), minus 2001:db8::/32 documentation.
    if ((bytes[0] & 0xE0) == 0x20) {
      if (bytes[0] == 0x20 && bytes[1] == 0x01 && bytes[2] == 0x0D && bytes[3] == 0xB8) return address_scope_e::other;
      return address_scope_e::global;
    }
    return address_scope_e::other;
  }

  std::optional<if_inet6_row_t>
  parse_if_inet6_line(std::string_view line) {
    // Format of net/ipv6/addrconf.c if6_seq_show: "%pi6 %02x %02x %02x %02x %8s" — address as 32 hex digits,
    // interface index, prefix length, scope, the low byte of the flags, name (linux-addrconf-v6.12).
    std::string text { line };
    std::istringstream stream { text };
    std::string hex, index, prefix, scope, flags, name;
    if (!(stream >> hex >> index >> prefix >> scope >> flags >> name)) return std::nullopt;
    if (hex.size() != 32 || !std::all_of(hex.begin(), hex.end(), [](unsigned char c) { return std::isxdigit(c); })) return std::nullopt;
    ip::address_v6::bytes_type bytes;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
      bytes[i] = static_cast<unsigned char>(std::stoul(hex.substr(i * 2, 2), nullptr, 16));
    }
    if_inet6_row_t row;
    try {
      row.address = ip::address_v6(bytes).to_string();
      row.index = static_cast<std::uint32_t>(std::stoul(index, nullptr, 16));
      row.prefix_length = static_cast<std::uint8_t>(std::stoul(prefix, nullptr, 16));
      row.flags = static_cast<std::uint8_t>(std::stoul(flags, nullptr, 16));
    }
    catch (const std::exception &) {
      return std::nullopt;
    }
    row.name = name;
    return row;
  }

  lifetime_e
  lifetime_from_windows(int suffix_origin, unsigned flags) {
    // IP_SUFFIX_ORIGIN: Other 0, Manual 1, WellKnown 2, Dhcp 3, LinkLayerAddress 4, Random 5 (ms-nl-suffix-origin-20261104).
    switch (suffix_origin) {
      case 1:
      case 2:
      case 3:
      case 4:
        return lifetime_e::stable;
      case 5:
        return (flags & WINDOWS_DNS_ELIGIBLE) ? lifetime_e::stable : lifetime_e::temporary;
      default:
        return lifetime_e::unknown;
    }
  }

  bool
  is_virtual_adapter(std::string_view value) {
    std::string name { value };
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const auto prefix : { "br-", "docker", "virbr", "veth", "lxc", "lxd", "cni", "flannel", "kube", "podman", "vmnet", "vboxnet", "vethernet", "tap", "tun" }) {
      if (name.rfind(prefix, 0) == 0) return true;
    }
    return false;
  }

  bool
  is_fake_ip(std::string_view address) {
    const auto parsed = parse_ip(address);
    return parsed && parsed->is_v4() && in_v4(parsed->to_v4(), 0xC6120000, 15);
  }

  std::string
  preferred_global(const std::vector<interface_t> &interfaces, int family) {
    const auto eligible = [](const interface_t &entry) {
      return entry.up && !entry.is_virtual && entry.mesh == reachability::mesh_kind_e::none;
    };
    const bool egress_known = std::any_of(interfaces.begin(), interfaces.end(), [&](const interface_t &entry) {
      return eligible(entry) && (family == 6 ? entry.egress_v6 : entry.egress_v4);
    });
    const interface_address_t *best = nullptr;
    int best_rank = 0;
    for (const auto &entry : interfaces) {
      if (!eligible(entry)) continue;
      if (egress_known && !(family == 6 ? entry.egress_v6 : entry.egress_v4)) continue;
      for (const auto &address : entry.addresses) {
        if (address.family != family || address.scope != address_scope_e::global) continue;
        // Larger is better: stable, then not deprecated.
        int rank = 1;
        if (address.lifetime == lifetime_e::stable) rank += 4;
        else if (address.lifetime == lifetime_e::unknown) rank += 2;
        if (!address.deprecated) rank += 1;
        if (rank > best_rank) {
          best_rank = rank;
          best = &address;
        }
      }
    }
    return best ? best->address : std::string {};
  }

  void
  summarize_family(family_discovery_t &family, const std::vector<std::string> &local_addresses) {
    family.public_address.clear();
    family.distinct_addresses.clear();
    for (const auto &server : family.servers) {
      if (!server.answered || !server.mapped) continue;
      if (family.public_address.empty()) family.public_address = server.mapped->address;
      if (!has(family.distinct_addresses, server.mapped->address)) family.distinct_addresses.push_back(server.mapped->address);
    }
    family.on_interface = !family.public_address.empty() && has(local_addresses, family.public_address);
  }

  reach_result_t
  assess_ipv4(const reachability::snapshot_t &snapshot, const discovery_t &discovery) {
    reach_result_t result;
    const auto &check = snapshot.public_reachability;
    result.reasons = check.reasons;
    switch (check.verdict) {
      case reachability::verdict_e::verified:
      case reachability::verdict_e::likely:
        result.verdict = reach_e::likely;
        break;
      case reachability::verdict_e::unreachable:
        result.verdict = reach_e::unreachable;
        break;
      case reachability::verdict_e::unknown:
        result.verdict = reach_e::unknown;
        break;
    }

    // The router's outside address is public, but the servers see another one: a second NAT (or a VPN or a
    // split-routing proxy) sits above the router, and its forwarding cannot be seen from here.
    const auto upstream = assess_upstream_nat(snapshot, discovery);
    if (upstream.state == "yes") {
      result.reasons.push_back("upstream_nat");
      if (result.verdict == reach_e::likely && !has(check.reasons, "session_evidence")) result.verdict = reach_e::unknown;
    }
    if (discovery.ipv4.on_interface) result.reasons.push_back("stun_on_interface");
    return result;
  }

  reach_result_t
  assess_ipv6(const reachability::snapshot_t &snapshot, const discovery_t &discovery) {
    reach_result_t result;
    const auto stable = preferred_global(discovery.interfaces, 6);
    if (stable.empty()) {
      result.verdict = reach_e::unreachable;
      result.reasons.push_back("ipv6_no_global");
      return result;
    }
    bool stable_found = false;
    for (const auto &entry : discovery.interfaces) {
      for (const auto &address : entry.addresses) {
        if (address.address == stable && address.lifetime == lifetime_e::stable) stable_found = true;
      }
    }
    result.reasons.push_back(stable_found ? "ipv6_stable_global" : "ipv6_only_temporary");

    bool listening_known = false;
    bool listening_v6 = true;
    bool blocked = false;
    for (const auto *port : client_tcp_ports(snapshot)) {
      if (port->firewall == reachability::firewall_e::blocked) {
        blocked = true;
        result.reasons.push_back("firewall_blocked:tcp/"s + std::to_string(port->port));
      }
      if (!port->listening) {
        listening_known = true;
        listening_v6 = false;
        continue;
      }
      if (port->bound.empty()) continue;
      listening_known = true;
      const bool any_v6 = std::any_of(port->bound.begin(), port->bound.end(), [](const std::string &address) {
        return address.find(':') != std::string::npos && address != "::1";
      });
      if (!any_v6) listening_v6 = false;
    }
    if (listening_known && !listening_v6) {
      result.verdict = reach_e::unreachable;
      result.reasons.push_back("ipv6_not_listening");
      return result;
    }
    if (blocked) {
      result.verdict = reach_e::unreachable;
      return result;
    }
    if (discovery.ipv6.public_address.empty()) {
      result.reasons.push_back("ipv6_stun_no_answer");
    }
    else if (!discovery.ipv6.on_interface) {
      // NPTv6 or NAT66: the servers see an address this host does not have.
      result.reasons.push_back("ipv6_translated");
      result.verdict = reach_e::unknown;
      return result;
    }
    // A client reached the host over IPv6 from the internet within the evidence window.
    for (const auto &entry : snapshot.public_reachability.evidence) {
      if (entry.address.find(':') != std::string::npos) {
        result.reasons.insert(result.reasons.begin(), "session_evidence");
        result.verdict = reach_e::likely;
        return result;
      }
    }
    result.reasons.push_back("ipv6_inbound_filter_unknown");
    result.verdict = reach_e::unknown;
    return result;
  }

  tri_state_t
  assess_cgnat(const reachability::snapshot_t &snapshot, const discovery_t &discovery) {
    tri_state_t result;
    const auto &router = snapshot.public_reachability.router;
    if (router.wan_ip_class == reachability::ip_class_e::cgnat) {
      result.state = "yes";
      result.reasons.push_back("wan_shared_address");
      return result;
    }
    if (discovery.ipv4.on_interface) {
      result.state = "no";
      result.reasons.push_back("stun_on_interface");
      return result;
    }
    if (router.wan_ip_class == reachability::ip_class_e::public_address) {
      if (!discovery.ipv4.public_address.empty() && discovery.ipv4.public_address == router.wan_ip) {
        result.state = "no";
        result.reasons.push_back("stun_matches_wan");
        return result;
      }
      if (discovery.ipv4.public_address.empty()) {
        result.state = "no";
        result.reasons.push_back("wan_public");
        return result;
      }
      // Public WAN but another reflexive address: something above the router, not necessarily a carrier.
      result.reasons.push_back("stun_differs_from_wan");
      return result;
    }
    if (router.wan_ip_class == reachability::ip_class_e::private_address) {
      result.reasons.push_back("wan_private");
      return result;
    }
    result.reasons.push_back(router.protocol == "none" ? "no_igd" : "no_wan_ip");
    return result;
  }

  tri_state_t
  assess_upstream_nat(const reachability::snapshot_t &snapshot, const discovery_t &discovery) {
    tri_state_t result;
    const auto &router = snapshot.public_reachability.router;
    if (router.wan_ip_class == reachability::ip_class_e::private_address || router.wan_ip_class == reachability::ip_class_e::cgnat) {
      result.state = "yes";
      result.reasons.push_back(router.wan_ip_class == reachability::ip_class_e::cgnat ? "wan_shared_address" : "wan_private");
      return result;
    }
    if (discovery.ipv4.on_interface) {
      result.state = "no";
      result.reasons.push_back("stun_on_interface");
      return result;
    }
    if (router.wan_ip_class == reachability::ip_class_e::public_address && !discovery.ipv4.public_address.empty()) {
      if (discovery.ipv4.public_address == router.wan_ip) {
        result.state = "no";
        result.reasons.push_back("stun_matches_wan");
      }
      else {
        result.state = "yes";
        result.reasons.push_back("stun_differs_from_wan");
      }
      return result;
    }
    result.reasons.push_back(discovery.ipv4.public_address.empty() ? "stun_no_answer" : (router.protocol == "none" ? "no_igd" : "no_wan_ip"));
    return result;
  }

  std::string_view
  to_string(mapping_e value) {
    switch (value) {
      case mapping_e::endpoint_independent:
        return "endpoint-independent";
      case mapping_e::address_dependent:
        return "address-dependent";
      case mapping_e::address_and_port_dependent:
        return "address-and-port-dependent";
      case mapping_e::endpoint_dependent:
        return "endpoint-dependent";
      case mapping_e::unknown:
        return "unknown";
    }
    return "unknown";
  }

  std::string_view
  to_string(method_e value) {
    switch (value) {
      case method_e::rfc5780:
        return "rfc5780";
      case method_e::two_servers:
        return "two-servers";
      case method_e::none:
        return "none";
    }
    return "none";
  }

  std::string_view
  to_string(address_scope_e value) {
    switch (value) {
      case address_scope_e::global:
        return "global";
      case address_scope_e::ula:
        return "ula";
      case address_scope_e::link_local:
        return "link-local";
      case address_scope_e::private_v4:
        return "private";
      case address_scope_e::cgnat:
        return "shared";
      case address_scope_e::loopback:
        return "loopback";
      case address_scope_e::other:
        return "other";
    }
    return "other";
  }

  std::string_view
  to_string(lifetime_e value) {
    switch (value) {
      case lifetime_e::stable:
        return "stable";
      case lifetime_e::temporary:
        return "temporary";
      case lifetime_e::unknown:
        return "unknown";
    }
    return "unknown";
  }

  std::string_view
  to_string(reach_e value) {
    switch (value) {
      case reach_e::likely:
        return "likely";
      case reach_e::unreachable:
        return "unreachable";
      case reach_e::unknown:
        return "unknown";
    }
    return "unknown";
  }

  namespace {
    nlohmann::json
    family_json(const family_discovery_t &family, const std::string &interface_global, bool enabled) {
      nlohmann::json servers = nlohmann::json::array();
      for (const auto &server : family.servers) {
        servers.push_back({
          { "server", server.server },
          { "address", server.address },
          { "answered", server.answered },
          { "error", server.error },
          { "mapped", transport_json(server.mapped) },
          { "other_address", transport_json(server.other) },
          { "rtt_ms", server.rtt_ms },
        });
      }
      nlohmann::json tests = nlohmann::json::array();
      for (const auto &test : family.tests) {
        tests.push_back({ { "test", test.name }, { "target", test.target }, { "mapped", transport_json(test.mapped) } });
      }
      const auto &mapping = family.mapping;
      nlohmann::json nat = {
        { "present", mapping.nat_present ? nlohmann::json(*mapping.nat_present) : nlohmann::json(nullptr) },
        { "mapping", to_string(mapping.mapping) },
        { "method", to_string(mapping.method) },
        { "mapped_address_varies", mapping.mapped_address_varies },
        { "filtering", "unknown" },
        { "filtering_reason", "not_tested" },
        { "reasons", mapping.reasons },
        { "tests", tests },
      };
      // The address to show. For IPv6 without translation it is the preferred stable interface address, even when the
      // servers saw a temporary one (a host that could not bind the stable address leaves via a temporary one).
      std::string shown = family.public_address;
      std::string source = shown.empty() ? "none" : "stun";
      if (shown.empty() && !interface_global.empty()) {
        shown = interface_global;
        source = "interface";
      }
      else if (family.family == 6 && family.on_interface && !interface_global.empty() && interface_global != shown) {
        shown = interface_global;
        source = "interface";
      }
      return {
        { "stun", enabled && family.socket_ok },
        { "local_address", family.local_address },
        { "public_address", shown },
        { "stun_address", family.public_address },
        { "source", source },
        { "on_interface", family.on_interface },
        { "consistent", family.distinct_addresses.size() <= 1 },
        { "seen_addresses", family.distinct_addresses },
        { "nat", nat },
        { "servers", servers },
        { "notes", family.notes },
      };
    }
  }  // namespace

  nlohmann::json
  overview_json(const discovery_t &discovery, const reachability::snapshot_t &snapshot) {
    nlohmann::json interfaces = nlohmann::json::array();
    for (const auto &entry : discovery.interfaces) {
      nlohmann::json addresses = nlohmann::json::array();
      for (const auto &address : entry.addresses) {
        nlohmann::json item = {
          { "address", address.address },
          { "family", address.family == 6 ? "ipv6" : "ipv4" },
          { "prefix_length", address.prefix_length },
          { "scope", to_string(address.scope) },
        };
        if (address.family == 6) {
          item["lifetime"] = to_string(address.lifetime);
          item["deprecated"] = address.deprecated;
        }
        addresses.push_back(std::move(item));
      }
      interfaces.push_back({
        { "name", entry.name },
        { "description", entry.description },
        { "up", entry.up },
        { "egress", entry.egress_v4 || entry.egress_v6 },
        { "virtual", entry.is_virtual },
        { "mesh", reachability::to_string(entry.mesh) },
        { "addresses", addresses },
      });
    }

    const auto &check = snapshot.public_reachability;
    nlohmann::json mappings = nlohmann::json::array();
    for (const auto &mapping : check.router.mappings) {
      mappings.push_back({ { "proto", mapping.proto }, { "port", mapping.port }, { "present", mapping.present }, { "internal", mapping.internal } });
    }
    nlohmann::json ports = nlohmann::json::array();
    std::vector<std::string> blocked;
    bool any_unknown = false;
    for (const auto &port : snapshot.ports) {
      ports.push_back({ { "proto", port.proto }, { "port", port.port }, { "role", port.role }, { "listening", port.listening },
        { "on_demand", reachability::binds_on_demand(port) }, { "firewall", reachability::to_string(port.firewall) } });
      if (port.firewall == reachability::firewall_e::blocked) blocked.push_back(port.proto + "/" + std::to_string(port.port));
      if (port.firewall == reachability::firewall_e::unknown) any_unknown = true;
    }
    std::string firewall_state = !blocked.empty() ? "blocked" : (any_unknown ? "unknown" : "allowed");

    const auto ipv4 = assess_ipv4(snapshot, discovery);
    const auto ipv6 = assess_ipv6(snapshot, discovery);
    const auto cgnat = assess_cgnat(snapshot, discovery);
    const auto upstream = assess_upstream_nat(snapshot, discovery);

    return {
      { "version", JSON_VERSION },
      { "checked_at", discovery.checked_at },
      { "reachability_checked_at", snapshot.checked_at },
      { "stun_enabled", discovery.enabled },
      { "stun", { { "enabled", discovery.enabled }, { "region", discovery.region }, { "contacted", discovery.contacted }, { "cache_seconds", CACHE_TTL_SECONDS } } },
      { "interfaces", interfaces },
      { "ipv4", family_json(discovery.ipv4, preferred_global(discovery.interfaces, 4), discovery.enabled) },
      { "ipv6", family_json(discovery.ipv6, preferred_global(discovery.interfaces, 6), discovery.enabled) },
      { "cgnat", { { "state", cgnat.state }, { "reasons", cgnat.reasons } } },
      { "upstream_nat", { { "state", upstream.state }, { "reasons", upstream.reasons } } },
      { "upnp", {
                  { "protocol", check.router.protocol },
                  { "available", check.router.protocol == "upnp-igd" },
                  { "wan_ip", check.router.wan_ip },
                  { "wan_ip_class", reachability::to_string(check.router.wan_ip_class) },
                  { "mappings", mappings },
                } },
      { "hairpin", { { "tried", check.hairpin.tried }, { "ok", check.hairpin.ok }, { "identity_match", check.hairpin.identity_match }, { "target", check.hairpin.target } } },
      { "firewall", { { "state", firewall_state }, { "blocked", blocked }, { "ports", ports } } },
      { "reachability", {
                          { "ipv4", { { "verdict", to_string(ipv4.verdict) }, { "reasons", ipv4.reasons } } },
                          { "ipv6", { { "verdict", to_string(ipv6.verdict) }, { "reasons", ipv6.reasons } } },
                          { "check_verdict", reachability::to_string(check.verdict) },
                        } },
    };
  }
}  // namespace nat_discovery
