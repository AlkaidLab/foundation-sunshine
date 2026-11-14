/**
 * @file src/network_reachability.h
 * @brief Host-side assessment of remote reachability over the internet and mesh overlays.
 *
 * The assessment uses only capabilities of the host itself: its listeners, its
 * adapters, its firewall rules and the gateway reachable on the local network.
 * No external checking service is contacted.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "public_address.h"

namespace reachability {
  /// Version of the JSON contract served by GET /api/network/reachability.
  constexpr int JSON_VERSION = 1;

  /// Evidence older than this is not treated as proof of current reachability.
  constexpr std::int64_t EVIDENCE_WINDOW_SECONDS = 7 * 24 * 60 * 60;

  /// Interval during which a cached snapshot is reused without probing again.
  constexpr std::int64_t CACHE_TTL_SECONDS = 60;

  /// Port offsets from a public method so the route module can name them.
  namespace port_role {
    constexpr std::string_view http = "http";
    constexpr std::string_view https = "https";
    constexpr std::string_view web = "web";
    constexpr std::string_view rtsp = "rtsp";
    constexpr std::string_view video = "video";
    constexpr std::string_view control = "control";
    constexpr std::string_view audio = "audio";
    constexpr std::string_view microphone = "microphone";
  }  // namespace port_role

  enum class verdict_e {
    verified,
    likely,
    unknown,
    unreachable,
  };

  enum class mesh_verdict_e {
    verified,
    likely,
    unknown,
    none,
  };

  enum class ip_class_e {
    public_address,
    private_address,
    cgnat,
    unknown,
  };

  enum class firewall_e {
    allowed,
    blocked,
    unknown,
  };

  enum class mesh_kind_e {
    netbird,
    tailscale,
    zerotier,
    easytier,
    wireguard,
    other,
    none,
  };

  struct port_status_t {
    std::string proto;  ///< "tcp" or "udp"
    std::uint16_t port = 0;
    std::string role;  ///< Stable role name, see port_role.
    bool listening = false;
    std::vector<std::string> bound;  ///< Local addresses the listener is bound to.
    firewall_e firewall = firewall_e::unknown;
  };

  struct mapping_t {
    std::string proto;
    std::uint16_t port = 0;
    bool present = false;
    std::string internal;  ///< Internal address the gateway forwards to; empty when absent.
  };

  struct router_status_t {
    std::string protocol = "none";  ///< "upnp-igd", "nat-pmp", "pcp" or "none"
    std::string wan_ip;
    ip_class_e wan_ip_class = ip_class_e::unknown;
    std::vector<mapping_t> mappings;
  };

  struct hairpin_status_t {
    bool tried = false;
    bool ok = false;
    std::uint16_t port = 0;
    bool identity_match = false;
    std::string target;  ///< Address the handshake was sent to; empty when not tried.
  };

  /// The operator-supplied public address (config external_ip) and what the
  /// host could confirm about it. `hint` carries the Web UI sentence when unset.
  struct configured_address_t {
    std::string value;
    public_address::kind_e kind = public_address::kind_e::empty;
    std::vector<std::string> resolved;  ///< System resolver answers; host names only.
    bool matches_wan = false;
    bool is_private = false;
    std::vector<std::string> reasons;  ///< address_mismatch, address_private, dns_failed, unset
    std::string hint;
  };

  /// Where an address sits: on the internet, in a private network, on a mesh overlay or on this computer.
  enum class scope_e {
    public_address,
    lan,
    mesh,
    loopback,
    unknown,
  };

  /// One TLS handshake attempt at an address and HTTPS port.
  struct handshake_t {
    bool connected = false;  ///< the TLS handshake completed
    bool identity_match = false;  ///< the certificate is this host's own
    std::string error;  ///< "", "refused", "timeout", "unreachable", "tls_failed" or "failed"
    std::int64_t latency_ms = 0;  ///< connect plus handshake time
  };

  struct endpoint_attempt_t {
    std::string address;
    std::uint16_t port = 0;  ///< HTTPS port that was tried
    handshake_t handshake;
  };

  /// What the host found out about one public endpoint of the configuration.
  struct endpoint_result_t {
    std::string input;  ///< canonical text as configured
    std::string host;
    public_address::kind_e kind = public_address::kind_e::empty;
    std::uint16_t port = 0;  ///< HTTP port: the given one, or the default
    bool port_explicit = false;
    std::uint16_t https_port = 0;  ///< port - 5; the port that was tested
    std::vector<std::string> resolved;  ///< addresses of a host name, or the IP literal itself
    scope_e scope = scope_e::unknown;
    bool matches_wan = false;
    bool reached = false;  ///< a handshake completed and the certificate is this host's own
    std::string error;  ///< why it failed: dns_failed, refused, timeout, unreachable, tls_failed, identity_mismatch or not_tried
    std::string tested_address;  ///< address of the attempt that decided the result; empty when none was made
    std::int64_t latency_ms = 0;  ///< latency of that attempt
    std::vector<endpoint_attempt_t> attempts;
    std::vector<std::string> reasons;  ///< address_mismatch, scope_lan, scope_loopback, scope_mesh
  };

  /// The system calls a probe of endpoints needs; the host supplies real ones, tests supply fakes.
  struct endpoint_prober_t {
    /// System resolver answers for a host name (empty on failure), bounded by the implementation.
    std::function<std::vector<std::string>(const std::string &host)> resolve;
    /// TLS handshake against `address` on `https_port`, bounded by the implementation.
    std::function<handshake_t(const std::string &address, std::uint16_t https_port)> handshake;
  };

  struct evidence_t {
    std::string kind;  ///< "client-connected-from-public" or "client-connected-from-mesh"
    std::string address;  ///< Masked address, never the full host address.
    std::string at;  ///< UTC timestamp, "YYYY-MM-DDTHH:MM:SSZ".
  };

  struct adapter_t {
    mesh_kind_e kind = mesh_kind_e::none;
    std::string name;
    std::vector<std::string> addresses;
    bool up = false;
  };

  struct public_inputs_t {
    std::vector<port_status_t> ports;
    std::string host_public_ip;  ///< Local interface address, empty when none.
    router_status_t router;
    hairpin_status_t hairpin;
    std::vector<evidence_t> evidence;
    bool web_ui_exposed_to_wan = false;
    configured_address_t configured;
    std::vector<endpoint_result_t> endpoints;  ///< when not empty these replace `configured` in the verdict
  };

  struct public_result_t {
    verdict_e verdict = verdict_e::unknown;
    std::vector<std::string> reasons;
    std::string host_public_ip;
    router_status_t router;
    hairpin_status_t hairpin;
    std::vector<evidence_t> evidence;
    configured_address_t configured;
    std::vector<endpoint_result_t> endpoints;
  };

  struct mesh_inputs_t {
    std::vector<port_status_t> ports;
    std::vector<adapter_t> adapters;
    bool origin_allows_mesh = false;
    std::vector<evidence_t> evidence;
  };

  struct mesh_result_t {
    std::vector<adapter_t> adapters;
    mesh_verdict_e verdict = mesh_verdict_e::none;
    std::vector<std::string> reasons;
    bool listening_on_mesh = false;
    bool web_ui_origin_allows_mesh = false;
    std::vector<evidence_t> evidence;
  };

  struct snapshot_t {
    int version = JSON_VERSION;
    std::string checked_at;
    std::vector<port_status_t> ports;
    public_result_t public_reachability;
    mesh_result_t mesh;
  };

  /// Inputs the host supplies to a probe run.
  struct probe_options_t {
    std::vector<port_status_t> ports;
    std::string certificate_path;  ///< PEM certificate the host serves; empty disables identity matching.
    std::string origin_web_ui_allowed { "lan" };  ///< "pc", "lan" or "wan"
    std::string external_ip;  ///< legacy config external_ip: an IP literal or a DNS host name.
    std::string external_endpoints;  ///< config external_endpoints: a list of host[:port]; wins over external_ip.
    std::uint16_t default_http_port = 0;  ///< HTTP port an endpoint without a port stands for.
    std::uint16_t hairpin_port = 0;
    std::uint32_t process_id = 0;  ///< Process whose listeners are inspected; 0 means this process.
  };

  /// True for the UDP stream ports, which this process binds when the first session starts.
  bool
  binds_on_demand(const port_status_t &port);

  /// Where an address sits; `unknown` for text that is not an address or is unspecified.
  scope_e
  scope_of(std::string_view address);

  /// The scope of an endpoint's addresses: public when any address is, otherwise mesh, lan or loopback.
  scope_e
  scope_of_all(const std::vector<std::string> &addresses);

  /**
   * @brief Probe every endpoint: resolve host names, then a TLS handshake against each address on the
   *        endpoint's HTTPS port (its HTTP port minus five). Endpoints run in parallel, so the time is
   *        bounded by one resolve plus one handshake, not by their number.
   */
  std::vector<endpoint_result_t>
  probe_endpoints(const std::vector<public_address::endpoint_t> &endpoints, std::uint16_t default_http_port,
    std::string_view wan_ip, const endpoint_prober_t &prober);

  /**
   * @brief The addresses of a one-off test (`GET /api/network/reachability?test=host[:port],...`) as the canonical list the probe takes:
   *        entries that do not parse and duplicates are dropped, at most public_address::MAX_ENDPOINTS are kept. Empty when nothing is valid.
   */
  std::string
  test_endpoints_option(std::string_view raw);

  /// Classify an address as a public, private or carrier-grade NAT address.
  ip_class_e
  classify_wan_ip(std::string_view address);

  /// True when the address belongs to a range commonly assigned by mesh overlays.
  bool
  is_mesh_address(std::string_view address);

  /// Identify a mesh overlay from an adapter name or description and its addresses.
  mesh_kind_e
  classify_adapter(std::string_view name, std::string_view description, const std::vector<std::string> &addresses);

  /// Mask an address for evidence (IPv4 /24, IPv6 /64).
  std::string
  mask_address(std::string_view address);

  /// Whether the configured Web UI origin policy admits an address.
  bool
  origin_allows(std::string_view origin, std::string_view address);

  /// Evaluate the public-internet verdict from collected host facts.
  public_result_t
  evaluate_public(const public_inputs_t &inputs, std::int64_t now_epoch_seconds);

  /// Evaluate the mesh verdict from collected host facts.
  mesh_result_t
  evaluate_mesh(const mesh_inputs_t &inputs, std::int64_t now_epoch_seconds);

  /**
   * @brief Validate the configured public address and compare it with the
   *        router WAN address. `resolved` is the system resolver result for a
   *        host name and is ignored for IP literals.
   */
  configured_address_t
  evaluate_configured_address(std::string_view configured_value, const std::vector<std::string> &resolved, std::string_view wan_ip);

  /// Render a snapshot as the versioned JSON contract.
  nlohmann::json
  to_json(const snapshot_t &snapshot);

  /// Parse the external address returned by an IGD WANIPConnection response.
  std::optional<std::string>
  parse_igd_external_ip(std::string_view raw);

  /// Interpret a UPNP_GetSpecificPortMappingEntry result.
  std::optional<mapping_t>
  parse_igd_mapping(int error_code, std::string_view internal_client, std::string_view lease_duration,
    std::string_view proto, std::uint16_t port);

  /// Format a UTC timestamp as "YYYY-MM-DDTHH:MM:SSZ".
  std::string
  format_utc_iso(std::int64_t epoch_seconds);

  /// Parse a timestamp produced by format_utc_iso.
  std::optional<std::int64_t>
  parse_utc_iso(std::string_view value);

  /// Current wall-clock time as seconds since the Unix epoch.
  std::int64_t
  now_epoch_seconds();

  /// Collect a snapshot from the host, reusing the cached result unless refresh is requested.
  snapshot_t
  probe(const probe_options_t &options, bool refresh);

  /// Record a client address observed on a completed session launch or resume.
  void
  observe_client(std::string_view address, std::int64_t epoch_seconds);

  /// Evidence collected by observe_client, newest first, masked.
  std::vector<evidence_t>
  observed_evidence(std::int64_t now_epoch_seconds);

  /// Stable string forms used in the JSON contract.
  std::string_view
  to_string(verdict_e value);
  std::string_view
  to_string(mesh_verdict_e value);
  std::string_view
  to_string(ip_class_e value);
  std::string_view
  to_string(firewall_e value);
  std::string_view
  to_string(mesh_kind_e value);
  std::string_view
  to_string(scope_e value);
}  // namespace reachability
