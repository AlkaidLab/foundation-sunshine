/**
 * @file src/nat_discovery.h
 * @brief Public addresses and NAT mapping behaviour learned with STUN, and the host's interface addresses.
 *
 * The host sends STUN Binding requests (RFC 8489) to public STUN servers and reads back the
 * reflexive transport address. The NAT mapping behaviour is determined with the tests of
 * RFC 5780 section 4.3, using the terms of RFC 4787 section 4.1. Filtering behaviour
 * (RFC 5780 section 4.4) is not determined: a public server that ignores CHANGE-REQUEST and a
 * NAT that filters the changed source look the same to the host, so filtering is reported
 * as unknown.
 *
 * Interface addresses carry the IPv6 temporary (RFC 8981) or stable distinction, and the
 * stable global address is preferred for display and reachability (RFC 6724 section 5, rule 7,
 * read in the opposite direction: a server wants the address that does not change).
 */
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "network_reachability.h"

namespace nat_discovery {
  /// Version of the JSON contract served by GET /api/network/overview.
  constexpr int JSON_VERSION = 1;

  /// Interval during which a cached discovery is reused without asking the servers again. STUN asks third-party
  /// servers, so it runs only when a person asks (the overview page, its recheck, the doctor) and is kept 10 minutes.
  constexpr std::int64_t CACHE_TTL_SECONDS = 10 * 60;

  /// At most this many servers are asked.
  constexpr std::size_t MAX_SERVERS = 8;

  /// The IANA default STUN port (RFC 8489 section 18.6).
  constexpr std::uint16_t DEFAULT_PORT = 3478;

  struct server_t {
    std::string host;
    std::uint16_t port = DEFAULT_PORT;
  };

  enum class region_e {
    china,  ///< a zh-CN locale or a China time zone: the mainland servers first, then Cloudflare
    global,  ///< anywhere else: Cloudflare first, then Google, then the mainland servers
  };

  /// The built-in list for a region; see docs/api/network-overview.md.
  std::vector<server_t>
  default_servers(region_e region);

  /**
   * @brief China when the host's locale is zh-CN (config `locale` zh, zh_CN or zh-Hans; zh_TW is not) or its
   *        time zone is a China one (IANA Asia/Shanghai, Asia/Chongqing, Asia/Harbin, Asia/Urumqi, PRC ...;
   *        Windows "China Standard Time").
   */
  region_e
  region_of(std::string_view locale, std::string_view time_zone);

  /// The host's time zone name: IANA from TZ or /etc/localtime on Linux and macOS, the Windows key name on Windows.
  std::string
  host_time_zone();

  struct server_list_t {
    std::vector<server_t> servers;
    std::vector<std::string> rejected;
    bool truncated = false;
    bool disabled = false;  ///< the configuration says "none"
    bool configured = false;  ///< the servers come from the configuration, not the built-in list
  };

  /// Parse config stun_servers: `host[:port]` items; empty selects the defaults of the region, "none" disables STUN.
  server_list_t
  parse_server_list(std::string_view text, region_e region = region_e::global);

  /// Canonical list text stored in the configuration.
  std::string
  join_servers(const std::vector<server_t> &servers);

  struct transport_address_t {
    std::string address;  ///< canonical IP text, no brackets
    std::uint16_t port = 0;

    bool
    operator==(const transport_address_t &) const = default;
  };

  /// `a.b.c.d:port` or `[v6]:port`.
  std::string
  format_transport(const transport_address_t &value);

  using transaction_id_t = std::array<std::uint8_t, 12>;

  /// A Binding request (RFC 8489 section 6.1) without attributes.
  std::vector<std::uint8_t>
  build_binding_request(const transaction_id_t &transaction_id);

  struct binding_response_t {
    bool success = false;  ///< a success response; false for an error response
    std::uint16_t error_code = 0;  ///< ERROR-CODE of an error response
    std::optional<transport_address_t> mapped;  ///< XOR-MAPPED-ADDRESS, or MAPPED-ADDRESS from an RFC 3489 server
    std::optional<transport_address_t> other;  ///< RFC 5780 OTHER-ADDRESS
    std::optional<transport_address_t> origin;  ///< RFC 5780 RESPONSE-ORIGIN
  };

  /// Decode a datagram; nullopt unless it is a valid Binding response to `transaction_id`.
  std::optional<binding_response_t>
  parse_binding_response(const std::uint8_t *data, std::size_t length, const transaction_id_t &transaction_id);

  // ---- mapping behaviour -----------------------------------------------------

  enum class mapping_e {
    endpoint_independent,  ///< RFC 4787 4.1: same mapping for every destination
    address_dependent,  ///< the mapping changes with the destination address
    address_and_port_dependent,  ///< the mapping changes with the destination address or port
    endpoint_dependent,  ///< it changes with the destination; the test that separates the two kinds could not run
    unknown,
  };

  enum class method_e {
    rfc5780,  ///< tests I-III against one server that returns OTHER-ADDRESS
    two_servers,  ///< test I and an equivalent of test II against two servers at different addresses
    none,
  };

  /// What the tests saw. A missing optional is a test that was not run or got no answer.
  struct mapping_inputs_t {
    method_e method = method_e::none;
    std::optional<transport_address_t> local;  ///< local address and port of the socket that sent test I
    std::vector<std::string> local_addresses;  ///< every address of this host, for the "not NATed" check
    std::optional<transport_address_t> test1;
    std::optional<transport_address_t> test2;
    bool test3_possible = false;  ///< the server offered an alternate port
    std::optional<transport_address_t> test3;
  };

  struct mapping_result_t {
    std::optional<bool> nat_present;  ///< empty when test I did not complete
    mapping_e mapping = mapping_e::unknown;
    method_e method = method_e::none;
    bool mapped_address_varies = false;  ///< tests saw different public IP addresses, not just ports
    std::vector<std::string> reasons;
  };

  /// The decision procedure of RFC 5780 section 4.3.
  mapping_result_t
  classify_mapping(const mapping_inputs_t &inputs);

  // ---- interface addresses ---------------------------------------------------

  enum class address_scope_e {
    global,  ///< public unicast
    ula,  ///< IPv6 fc00::/7 (RFC 4193)
    link_local,  ///< IPv6 fe80::/10, IPv4 169.254/16
    private_v4,  ///< RFC 1918
    cgnat,  ///< 100.64.0.0/10 (RFC 6598)
    loopback,
    other,  ///< multicast, documentation, unspecified, not an address
  };

  /// Where an address sits; textual input, scope id allowed.
  address_scope_e
  classify_address(std::string_view address);

  enum class lifetime_e {
    stable,  ///< does not rotate: SLAAC with a stable identifier, DHCPv6, manual
    temporary,  ///< RFC 8981 temporary address
    unknown,  ///< the platform does not say
  };

  struct interface_address_t {
    std::string address;
    int family = 4;  ///< 4 or 6
    std::uint8_t prefix_length = 0;
    address_scope_e scope = address_scope_e::other;
    lifetime_e lifetime = lifetime_e::unknown;  ///< IPv6 only; IPv4 stays unknown
    bool deprecated = false;  ///< the preferred lifetime has ended
  };

  struct interface_t {
    std::string name;
    std::string description;
    std::uint32_t index = 0;
    bool up = false;
    bool egress_v4 = false;  ///< the host sends to the IPv4 internet from an address of this interface
    bool egress_v6 = false;  ///< the same for IPv6
    bool is_virtual = false;  ///< a container, VM or tunnel bridge by its name (docker, virbr, veth, vEthernet ...)
    reachability::mesh_kind_e mesh = reachability::mesh_kind_e::none;
    std::vector<interface_address_t> addresses;
  };

  /// One row of Linux /proc/net/if_inet6.
  struct if_inet6_row_t {
    std::string address;
    std::uint32_t index = 0;
    std::uint8_t prefix_length = 0;
    std::uint8_t flags = 0;  ///< low byte of the IFA_F_* flags
    std::string name;
  };

  std::optional<if_inet6_row_t>
  parse_if_inet6_line(std::string_view line);

  /// IFA_F_TEMPORARY and IFA_F_DEPRECATED from include/uapi/linux/if_addr.h.
  constexpr std::uint8_t IFA_TEMPORARY = 0x01;
  constexpr std::uint8_t IFA_DEPRECATED = 0x20;

  /// IP_ADAPTER_ADDRESS_DNS_ELIGIBLE from iptypes.h.
  constexpr unsigned WINDOWS_DNS_ELIGIBLE = 0x01;

  /**
   * @brief Lifetime of a Windows unicast address from IP_ADAPTER_UNICAST_ADDRESS SuffixOrigin
   *        (IP_SUFFIX_ORIGIN, nldef.h) and Flags. Windows also randomizes the identifier of its
   *        stable address, so a random suffix alone is not enough: a temporary address is the one
   *        with a random suffix that may not appear in DNS.
   */
  lifetime_e
  lifetime_from_windows(int suffix_origin, unsigned flags);

  /// True for adapter names of container, VM and tunnel bridges.
  bool
  is_virtual_adapter(std::string_view name);

  /**
   * @brief The address to show and to test for a family: a global address on an up interface
   *        that is neither a mesh overlay nor a virtual bridge, stable before temporary,
   *        preferred before deprecated. When the host knows its egress interface for the
   *        family, only that interface counts. Empty when there is none.
   */
  std::string
  preferred_global(const std::vector<interface_t> &interfaces, int family);

  // ---- discovery -------------------------------------------------------------

  struct server_observation_t {
    std::string server;  ///< host:port as configured
    std::string address;  ///< server IP that was asked; empty when it did not resolve
    bool answered = false;
    std::string error;  ///< "", "resolve_failed", "other_family", "no_route", "timeout", "send_failed", "error_response"
    std::optional<transport_address_t> mapped;
    std::optional<transport_address_t> other;
    std::int64_t rtt_ms = 0;
  };

  struct mapping_test_t {
    std::string name;  ///< "I", "II" or "III"
    std::string target;  ///< host:port the request went to
    std::optional<transport_address_t> mapped;
  };

  struct family_discovery_t {
    int family = 4;
    bool socket_ok = false;  ///< a socket of the family could be opened and the host has a route
    std::string local_address;  ///< source address the host uses towards the internet
    std::string public_address;  ///< the first answer, in list order
    bool on_interface = false;  ///< the public address is one of this host's own addresses
    std::vector<std::string> distinct_addresses;  ///< all public addresses the servers reported
    mapping_result_t mapping;
    std::vector<mapping_test_t> tests;
    std::vector<server_observation_t> servers;
    std::vector<std::string> notes;  ///< "dns_fake_ip": the resolver answered from 198.18.0.0/15, a proxy's fake-IP pool
  };

  /// True for 198.18.0.0/15 (RFC 2544 benchmarking), the range proxy "fake-IP" DNS modes hand out.
  bool
  is_fake_ip(std::string_view address);

  struct discovery_t {
    std::string checked_at;
    bool enabled = true;  ///< false when config stun_servers is "none"
    std::string region;  ///< "china", "global" or "configured": where the server order came from
    std::vector<std::string> contacted;  ///< every server host:port this run asked, in order
    std::vector<interface_t> interfaces;
    family_discovery_t ipv4 { 4 };
    family_discovery_t ipv6 { 6 };
  };

  /// Combine a family's observations into its public address, its consistency and its mapping inputs.
  void
  summarize_family(family_discovery_t &family, const std::vector<std::string> &local_addresses);

  // ---- overview --------------------------------------------------------------

  enum class reach_e {
    likely,  ///< 可能可达
    unreachable,  ///< 不可达
    unknown,  ///< 未知
  };

  struct reach_result_t {
    reach_e verdict = reach_e::unknown;
    std::vector<std::string> reasons;
  };

  /// The public verdict of the reachability check, restated with STUN facts. Nothing is "verified":
  /// none of the checks is a connection made from outside right now.
  reach_result_t
  assess_ipv4(const reachability::snapshot_t &snapshot, const discovery_t &discovery);

  /// IPv6 needs a stable global address, a listener on IPv6 and an open firewall. The router's stateful
  /// IPv6 filter (RFC 6092: only flows started inside pass, unless the gateway runs in the "transparent
  /// mode" of section 3.4) cannot be seen from the host, so without a past session the best is unknown.
  reach_result_t
  assess_ipv6(const reachability::snapshot_t &snapshot, const discovery_t &discovery);

  /// "yes", "no" or "unknown", with reason codes.
  struct tri_state_t {
    std::string state = "unknown";
    std::vector<std::string> reasons;
  };

  tri_state_t
  assess_cgnat(const reachability::snapshot_t &snapshot, const discovery_t &discovery);

  tri_state_t
  assess_upstream_nat(const reachability::snapshot_t &snapshot, const discovery_t &discovery);

  /// The JSON contract of GET /api/network/overview, version JSON_VERSION.
  nlohmann::json
  overview_json(const discovery_t &discovery, const reachability::snapshot_t &snapshot);

  /// Stable string forms used in the contract.
  std::string_view
  to_string(mapping_e value);
  std::string_view
  to_string(method_e value);
  std::string_view
  to_string(address_scope_e value);
  std::string_view
  to_string(lifetime_e value);
  std::string_view
  to_string(reach_e value);

  // ---- host probe (nat_discovery_probe.cpp) ----------------------------------

  struct discover_options_t {
    std::vector<server_t> servers;
    bool enabled = true;
    std::string region;  ///< copied into discovery_t::region
  };

  /// Enumerate interfaces and ask the servers, reusing the cached result unless refresh is requested.
  discovery_t
  discover(const discover_options_t &options, bool refresh);

  /// Interface enumeration only; no network traffic.
  std::vector<interface_t>
  enumerate_interfaces();
}  // namespace nat_discovery
