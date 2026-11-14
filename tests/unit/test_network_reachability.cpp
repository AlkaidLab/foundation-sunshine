#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "src/network_reachability.h"

namespace {
  using namespace reachability;

  constexpr std::int64_t NOW = 1767225600;  // 2026-01-01T00:00:00Z

  port_status_t
  make_port(std::string proto, std::uint16_t port, std::string role, bool listening = true,
    firewall_e firewall = firewall_e::allowed, std::vector<std::string> bound = { "0.0.0.0" }) {
    return { std::move(proto), port, std::move(role), listening, std::move(bound), firewall };
  }

  std::vector<port_status_t>
  healthy_ports(bool listening = true, firewall_e firewall = firewall_e::allowed,
    std::vector<std::string> bound = { "0.0.0.0" }) {
    return {
      make_port("tcp", 47984, "https", listening, firewall, bound),
      make_port("tcp", 47989, "http", listening, firewall, bound),
      make_port("tcp", 47990, "web", listening, firewall, bound),
      make_port("tcp", 48010, "rtsp", listening, firewall, bound),
      make_port("udp", 47998, "video", listening, firewall, bound),
      make_port("udp", 47999, "control", listening, firewall, bound),
      make_port("udp", 48000, "audio", listening, firewall, bound),
      make_port("udp", 48001, "microphone", listening, firewall, bound),
    };
  }

  router_status_t
  public_router(bool mappings_present) {
    router_status_t router;
    router.protocol = "upnp-igd";
    router.wan_ip = "203.0.113.9";
    router.wan_ip_class = ip_class_e::public_address;
    for (const auto &port : healthy_ports()) {
      if (port.role == "web") continue;
      router.mappings.push_back({ port.proto, port.port, mappings_present, mappings_present ? "10.0.1.10" : "" });
    }
    return router;
  }

  bool
  has_reason(const std::vector<std::string> &reasons, const std::string &value) {
    for (const auto &reason : reasons) {
      if (reason == value) return true;
    }
    return false;
  }
}  // namespace

TEST(NetworkReachabilityClassification, WanAddressClasses) {
  EXPECT_EQ(classify_wan_ip("8.8.8.8"), ip_class_e::public_address);
  EXPECT_EQ(classify_wan_ip("10.0.1.20"), ip_class_e::private_address);
  EXPECT_EQ(classify_wan_ip("10.2.3.4"), ip_class_e::private_address);
  EXPECT_EQ(classify_wan_ip("172.20.1.1"), ip_class_e::private_address);
  EXPECT_EQ(classify_wan_ip("100.64.12.3"), ip_class_e::cgnat);
  EXPECT_EQ(classify_wan_ip("2606:4700:4700::1111"), ip_class_e::public_address);
  EXPECT_EQ(classify_wan_ip("fd00::1"), ip_class_e::private_address);
  EXPECT_EQ(classify_wan_ip("fe80::1"), ip_class_e::private_address);
  EXPECT_EQ(classify_wan_ip("0.0.0.0"), ip_class_e::unknown);
  EXPECT_EQ(classify_wan_ip("not-an-address"), ip_class_e::unknown);
}

TEST(NetworkReachabilityClassification, MasksEvidenceAddresses) {
  EXPECT_EQ(mask_address("203.0.113.45"), "203.0.113.0/24");
  EXPECT_EQ(mask_address("100.101.102.103"), "100.101.102.0/24");
  EXPECT_EQ(mask_address("fd7a:115c:a1e0:ab12::1"), "fd7a:115c:a1e0::/64");
  EXPECT_EQ(mask_address("invalid"), "");
}

TEST(NetworkReachabilityClassification, OriginPolicyMatchesThisTree) {
  // This tree classifies 100.64.0.0/10 as LAN, so a LAN origin admits mesh addresses.
  EXPECT_TRUE(origin_allows("lan", "100.64.0.5"));
  EXPECT_TRUE(origin_allows("wan", "100.64.0.5"));
  EXPECT_FALSE(origin_allows("pc", "100.64.0.5"));
  EXPECT_FALSE(origin_allows("pc", "10.0.1.5"));
  EXPECT_TRUE(origin_allows("lan", "10.0.1.5"));
  EXPECT_FALSE(origin_allows("lan", "8.8.8.8"));
  EXPECT_TRUE(origin_allows("wan", "8.8.8.8"));
}

TEST(NetworkReachabilityClassification, IdentifiesMeshAdapters) {
  EXPECT_EQ(classify_adapter("tailscale0", "", { "100.100.1.2" }), mesh_kind_e::tailscale);
  EXPECT_EQ(classify_adapter("wt0", "", { "100.100.1.2" }), mesh_kind_e::netbird);
  EXPECT_EQ(classify_adapter("Ethernet", "NetBird Tunnel", { "100.100.1.2" }), mesh_kind_e::netbird);
  EXPECT_EQ(classify_adapter("ztabc", "", { "10.147.20.4" }), mesh_kind_e::zerotier);
  EXPECT_EQ(classify_adapter("wg0", "", { "10.9.0.2" }), mesh_kind_e::wireguard);
  EXPECT_EQ(classify_adapter("Ethernet", "Realtek", { "10.0.1.2" }), mesh_kind_e::none);
  EXPECT_EQ(classify_adapter("tun9", "", { "100.64.7.7" }), mesh_kind_e::other);
  EXPECT_TRUE(is_mesh_address("100.64.3.2"));
  EXPECT_TRUE(is_mesh_address("fd7a:115c:a1e0::5"));
  EXPECT_FALSE(is_mesh_address("10.0.1.2"));
}

TEST(NetworkReachabilityPublic, RecentPublicSessionIsVerified) {
  public_inputs_t inputs;
  inputs.ports = healthy_ports();
  inputs.evidence.push_back({ "client-connected-from-public", "203.0.113.0/24", format_utc_iso(NOW - 3600) });
  const auto result = evaluate_public(inputs, NOW);
  EXPECT_EQ(result.verdict, verdict_e::verified);
  EXPECT_TRUE(has_reason(result.reasons, "session_evidence"));
  ASSERT_EQ(result.evidence.size(), 1U);
  EXPECT_EQ(result.evidence.front().address, "203.0.113.0/24");
}

TEST(NetworkReachabilityPublic, OldSessionEvidenceIsIgnored) {
  public_inputs_t inputs;
  inputs.ports = healthy_ports();
  inputs.evidence.push_back({ "client-connected-from-public", "203.0.113.0/24", format_utc_iso(NOW - EVIDENCE_WINDOW_SECONDS - 60) });
  inputs.router = public_router(true);
  const auto result = evaluate_public(inputs, NOW);
  EXPECT_EQ(result.verdict, verdict_e::likely);
  EXPECT_FALSE(has_reason(result.reasons, "session_evidence"));
}

TEST(NetworkReachabilityPublic, MissingListenerIsUnreachable) {
  public_inputs_t inputs;
  inputs.ports = healthy_ports(false);
  const auto result = evaluate_public(inputs, NOW);
  EXPECT_EQ(result.verdict, verdict_e::unreachable);
  EXPECT_TRUE(has_reason(result.reasons, "not_listening:tcp/47989"));
  // The UDP stream sockets exist only while a session runs: an idle host is not "not listening" on them.
  EXPECT_FALSE(has_reason(result.reasons, "not_listening:udp/47998"));
}

TEST(NetworkReachabilityPublic, IdleUdpStreamPortsDoNotMakeTheHostUnreachable) {
  public_inputs_t inputs;
  inputs.ports = healthy_ports();
  for (auto &port : inputs.ports) {
    if (port.proto == "udp") {
      port.listening = false;
      port.bound.clear();
    }
  }
  inputs.host_public_ip = "203.0.113.9";
  const auto result = evaluate_public(inputs, NOW);
  EXPECT_NE(result.verdict, verdict_e::unreachable);
  for (const auto &reason : result.reasons) EXPECT_EQ(reason.rfind("not_listening", 0), std::string::npos) << reason;
}

TEST(NetworkReachabilityPublic, BlockedFirewallIsUnreachable) {
  public_inputs_t inputs;
  inputs.ports = healthy_ports();
  inputs.ports.at(1).firewall = firewall_e::blocked;
  const auto result = evaluate_public(inputs, NOW);
  EXPECT_EQ(result.verdict, verdict_e::unreachable);
  EXPECT_TRUE(has_reason(result.reasons, "firewall_blocked:tcp/47989"));
}

TEST(NetworkReachabilityPublic, CgnatWanIsUnreachable) {
  public_inputs_t inputs;
  inputs.ports = healthy_ports();
  inputs.configured = evaluate_configured_address("203.0.113.9", {}, "");
  inputs.router.protocol = "upnp-igd";
  inputs.router.wan_ip = "100.64.3.4";
  inputs.router.wan_ip_class = ip_class_e::cgnat;
  const auto result = evaluate_public(inputs, NOW);
  EXPECT_EQ(result.verdict, verdict_e::unreachable);
  EXPECT_TRUE(has_reason(result.reasons, "cgnat"));
}

TEST(NetworkReachabilityPublic, PrivateWanIsUnreachable) {
  public_inputs_t inputs;
  inputs.ports = healthy_ports();
  inputs.configured = evaluate_configured_address("203.0.113.9", {}, "");
  inputs.router.protocol = "upnp-igd";
  inputs.router.wan_ip = "10.0.1.1";
  inputs.router.wan_ip_class = ip_class_e::private_address;
  const auto result = evaluate_public(inputs, NOW);
  EXPECT_EQ(result.verdict, verdict_e::unreachable);
  EXPECT_TRUE(has_reason(result.reasons, "private_wan"));
}

TEST(NetworkReachabilityPublic, UnsetAddressIsAHintEvenBehindCarrierNat) {
  public_inputs_t inputs;
  inputs.ports = healthy_ports();
  inputs.router.protocol = "upnp-igd";
  inputs.router.wan_ip = "100.64.3.4";
  inputs.router.wan_ip_class = ip_class_e::cgnat;
  const auto result = evaluate_public(inputs, NOW);
  EXPECT_EQ(result.verdict, verdict_e::unknown);
  EXPECT_TRUE(has_reason(result.reasons, "unset"));
  EXPECT_TRUE(has_reason(result.reasons, "cgnat"));
}

TEST(NetworkReachabilityPublic, UnsetAddressBehindPrivateWanIsNotCalledAFailure) {
  public_inputs_t inputs;
  inputs.ports = healthy_ports();
  inputs.router.protocol = "upnp-igd";
  inputs.router.wan_ip = "10.0.1.1";
  inputs.router.wan_ip_class = ip_class_e::private_address;
  const auto result = evaluate_public(inputs, NOW);
  EXPECT_EQ(result.verdict, verdict_e::unknown);
  EXPECT_TRUE(has_reason(result.reasons, "unset"));
  EXPECT_TRUE(has_reason(result.reasons, "private_wan"));
}

TEST(NetworkReachabilityPublic, NoIgdLeavesVerdictUnknown) {
  public_inputs_t inputs;
  inputs.ports = healthy_ports();
  const auto result = evaluate_public(inputs, NOW);
  EXPECT_EQ(result.verdict, verdict_e::unknown);
  EXPECT_TRUE(has_reason(result.reasons, "no_igd"));
}

TEST(NetworkReachabilityPublic, IgdWithoutWanAddressIsReportedSeparately) {
  public_inputs_t inputs;
  inputs.ports = healthy_ports();
  inputs.router.protocol = "upnp-igd";
  const auto result = evaluate_public(inputs, NOW);
  EXPECT_EQ(result.verdict, verdict_e::unknown);
  EXPECT_TRUE(has_reason(result.reasons, "no_wan_ip"));
}

TEST(NetworkReachabilityPublic, HairpinThroughOurCertificateIsVerified) {
  public_inputs_t inputs;
  inputs.ports = healthy_ports();
  inputs.router = public_router(true);
  inputs.hairpin = { true, true, 47984, true };
  const auto result = evaluate_public(inputs, NOW);
  EXPECT_EQ(result.verdict, verdict_e::verified);
  EXPECT_TRUE(has_reason(result.reasons, "hairpin_verified"));
  EXPECT_TRUE(has_reason(result.reasons, "mappings_complete"));
}

TEST(NetworkReachabilityPublic, CompleteMappingsWithoutHairpinAreLikely) {
  public_inputs_t inputs;
  inputs.ports = healthy_ports();
  inputs.router = public_router(true);
  const auto result = evaluate_public(inputs, NOW);
  EXPECT_EQ(result.verdict, verdict_e::likely);
  EXPECT_TRUE(has_reason(result.reasons, "hairpin_not_tried"));
}

TEST(NetworkReachabilityPublic, MissingMappingIsReported) {
  public_inputs_t inputs;
  inputs.ports = healthy_ports();
  inputs.router = public_router(true);
  for (auto &mapping : inputs.router.mappings) {
    if (mapping.port == 47998 && mapping.proto == "udp") mapping.present = false;
  }
  const auto result = evaluate_public(inputs, NOW);
  EXPECT_EQ(result.verdict, verdict_e::unknown);
  EXPECT_TRUE(has_reason(result.reasons, "mapping_missing:udp/47998"));
}

TEST(NetworkReachabilityPublic, HostPublicAddressWithFirewallIsVerified) {
  public_inputs_t inputs;
  inputs.ports = healthy_ports();
  inputs.host_public_ip = "203.0.113.9";
  const auto result = evaluate_public(inputs, NOW);
  EXPECT_EQ(result.verdict, verdict_e::verified);
  EXPECT_TRUE(has_reason(result.reasons, "host_public_ip"));
}

TEST(NetworkReachabilityPublic, WebPortIsOnlyRequiredWhenExposed) {
  public_inputs_t inputs;
  inputs.ports = healthy_ports();
  inputs.ports.at(2).listening = false;  // The Web UI port is not required on LAN.
  inputs.router = public_router(true);
  const auto result = evaluate_public(inputs, NOW);
  EXPECT_EQ(result.verdict, verdict_e::likely);

  public_inputs_t exposed;
  exposed.ports = inputs.ports;
  exposed.web_ui_exposed_to_wan = true;
  exposed.router = inputs.router;
  const auto exposed_result = evaluate_public(exposed, NOW);
  EXPECT_EQ(exposed_result.verdict, verdict_e::unreachable);
  EXPECT_TRUE(has_reason(exposed_result.reasons, "not_listening:tcp/47990"));
}

TEST(NetworkReachabilityMesh, NoAdapterMeansNone) {
  mesh_inputs_t inputs;
  inputs.ports = healthy_ports();
  const auto result = evaluate_mesh(inputs, NOW);
  EXPECT_EQ(result.verdict, mesh_verdict_e::none);
  EXPECT_TRUE(has_reason(result.reasons, "no_mesh_adapter"));
}

TEST(NetworkReachabilityMesh, WildcardListenerOnUpAdapterIsLikely) {
  mesh_inputs_t inputs;
  inputs.ports = healthy_ports();
  inputs.adapters.push_back({ mesh_kind_e::tailscale, "tailscale0", { "100.100.1.2" }, true });
  inputs.origin_allows_mesh = true;
  const auto result = evaluate_mesh(inputs, NOW);
  EXPECT_EQ(result.verdict, mesh_verdict_e::likely);
  EXPECT_TRUE(result.listening_on_mesh);
  EXPECT_TRUE(result.web_ui_origin_allows_mesh);
}

TEST(NetworkReachabilityMesh, ListenerBoundToLanOnlyIsNotReachableOnMesh) {
  mesh_inputs_t inputs;
  inputs.ports = healthy_ports(true, firewall_e::unknown, { "10.0.1.10" });
  inputs.adapters.push_back({ mesh_kind_e::tailscale, "tailscale0", { "100.100.1.2" }, true });
  inputs.origin_allows_mesh = true;
  const auto result = evaluate_mesh(inputs, NOW);
  EXPECT_EQ(result.verdict, mesh_verdict_e::unknown);
  EXPECT_FALSE(result.listening_on_mesh);
  EXPECT_TRUE(has_reason(result.reasons, "not_listening_on_mesh:tcp/47984"));
}

TEST(NetworkReachabilityMesh, OriginPolicyIsReported) {
  mesh_inputs_t inputs;
  inputs.ports = healthy_ports();
  inputs.adapters.push_back({ mesh_kind_e::netbird, "wt0", { "100.100.1.2" }, true });
  inputs.origin_allows_mesh = false;
  const auto result = evaluate_mesh(inputs, NOW);
  EXPECT_EQ(result.verdict, mesh_verdict_e::likely);
  EXPECT_FALSE(result.web_ui_origin_allows_mesh);
  EXPECT_TRUE(has_reason(result.reasons, "origin_blocks_mesh"));
}

TEST(NetworkReachabilityMesh, RecentMeshSessionIsVerified) {
  mesh_inputs_t inputs;
  inputs.ports = healthy_ports();
  inputs.adapters.push_back({ mesh_kind_e::tailscale, "tailscale0", { "100.100.1.2" }, true });
  inputs.origin_allows_mesh = true;
  inputs.evidence.push_back({ "client-connected-from-mesh", "100.100.1.0/24", format_utc_iso(NOW - 120) });
  const auto result = evaluate_mesh(inputs, NOW);
  EXPECT_EQ(result.verdict, mesh_verdict_e::verified);
  EXPECT_TRUE(has_reason(result.reasons, "session_evidence"));
}

TEST(NetworkReachabilityIgd, ParsesExternalAddressResponses) {
  ASSERT_TRUE(parse_igd_external_ip("203.0.113.9").has_value());
  EXPECT_EQ(*parse_igd_external_ip("203.0.113.9"), "203.0.113.9");
  EXPECT_FALSE(parse_igd_external_ip("").has_value());
  EXPECT_FALSE(parse_igd_external_ip("0.0.0.0").has_value());
  EXPECT_FALSE(parse_igd_external_ip("garbage").has_value());
}

TEST(NetworkReachabilityIgd, ParsesPortMappingResponses) {
  const auto missing = parse_igd_mapping(714, "", "0", "udp", 47998);
  ASSERT_TRUE(missing.has_value());
  EXPECT_FALSE(missing->present);

  const auto present = parse_igd_mapping(0, "10.0.1.10", "3600", "udp", 47998);
  ASSERT_TRUE(present.has_value());
  EXPECT_TRUE(present->present);
  EXPECT_EQ(present->internal, "10.0.1.10");
  EXPECT_EQ(present->proto, "udp");
  EXPECT_EQ(present->port, 47998);

  EXPECT_FALSE(parse_igd_mapping(606, "10.0.1.10", "0", "tcp", 47989).has_value());
  const auto empty_client = parse_igd_mapping(0, "", "0", "tcp", 47989);
  ASSERT_TRUE(empty_client.has_value());
  EXPECT_FALSE(empty_client->present);
}

TEST(NetworkReachabilityJson, ContractShapeIsStable) {
  snapshot_t snapshot;
  snapshot.checked_at = format_utc_iso(NOW);
  snapshot.ports = healthy_ports();
  public_inputs_t public_inputs;
  public_inputs.ports = snapshot.ports;
  public_inputs.configured = evaluate_configured_address("", {}, "");
  snapshot.public_reachability = evaluate_public(public_inputs, NOW);
  mesh_inputs_t mesh_inputs;
  mesh_inputs.ports = snapshot.ports;
  snapshot.mesh = evaluate_mesh(mesh_inputs, NOW);

  const auto json = to_json(snapshot);
  EXPECT_EQ(json.at("version"), 1);
  EXPECT_EQ(json.at("checked_at"), "2026-01-01T00:00:00Z");
  ASSERT_EQ(json.at("ports").size(), 8U);
  EXPECT_EQ(json.at("ports").at(0).at("proto"), "tcp");
  EXPECT_EQ(json.at("ports").at(0).at("port"), 47984);
  EXPECT_EQ(json.at("ports").at(0).at("role"), "https");
  EXPECT_EQ(json.at("ports").at(0).at("firewall"), "allowed");
  EXPECT_EQ(json.at("public").at("verdict"), "unknown");
  EXPECT_EQ(json.at("public").at("router").at("protocol"), "none");
  EXPECT_EQ(json.at("public").at("hairpin").at("tried"), false);
  EXPECT_TRUE(json.at("public").at("hairpin").at("target").is_null());
  EXPECT_EQ(json.at("public").at("configured_address").at("kind"), "empty");
  EXPECT_TRUE(json.at("public").at("configured_address").at("value").is_null());
  EXPECT_EQ(json.at("public").at("configured_address").at("hint"), "未设置公网地址时客户端使用自动发现的地址");
  EXPECT_EQ(json.at("public").at("configured_address").at("reasons").at(0), "unset");
  EXPECT_EQ(json.at("mesh").at("verdict"), "none");
  EXPECT_EQ(json.at("mesh").at("listening_on_mesh"), false);
  EXPECT_TRUE(json.at("public").at("reasons").is_array());
  EXPECT_TRUE(json.at("mesh").at("adapters").is_array());
}

TEST(NetworkReachabilityEvidence, RecordsOnlyPublicAndMeshClients) {
  observe_client("203.0.113.45", NOW);
  observe_client("10.0.1.20", NOW);
  observe_client("100.100.2.9", NOW);
  observe_client("not-an-address", NOW);

  const auto evidence = observed_evidence(NOW);
  ASSERT_EQ(evidence.size(), 2U);
  EXPECT_EQ(evidence.at(0).kind, "client-connected-from-mesh");
  EXPECT_EQ(evidence.at(0).address, "100.100.2.0/24");
  EXPECT_EQ(evidence.at(1).kind, "client-connected-from-public");
  EXPECT_EQ(evidence.at(1).address, "203.0.113.0/24");
}

TEST(NetworkReachabilityEvidence, UtcTimestampsRoundTrip) {
  EXPECT_EQ(format_utc_iso(NOW), "2026-01-01T00:00:00Z");
  ASSERT_TRUE(parse_utc_iso("2026-01-01T00:00:00Z").has_value());
  EXPECT_EQ(*parse_utc_iso("2026-01-01T00:00:00Z"), NOW);
  EXPECT_FALSE(parse_utc_iso("2026-01-01 00:00:00").has_value());
  EXPECT_FALSE(parse_utc_iso("2026-13-01T00:00:00Z").has_value());
}

TEST(PublicAddressSyntax, AcceptsIpLiteralsAndDnsNames) {
  EXPECT_EQ(public_address::classify("203.0.113.9"), public_address::kind_e::ip);
  EXPECT_EQ(public_address::classify("2001:db8::9"), public_address::kind_e::ip);
  EXPECT_EQ(public_address::classify("example.ddns.net"), public_address::kind_e::hostname);
  EXPECT_EQ(public_address::classify("host-1.example.com."), public_address::kind_e::hostname);
  EXPECT_EQ(public_address::classify("  example.ddns.net  "), public_address::kind_e::hostname);
  EXPECT_EQ(public_address::classify(""), public_address::kind_e::empty);
}

TEST(PublicAddressSyntax, RejectsMalformedValues) {
  EXPECT_EQ(public_address::classify("123.456.789.12"), public_address::kind_e::invalid);
  EXPECT_EQ(public_address::classify("host name"), public_address::kind_e::invalid);
  EXPECT_EQ(public_address::classify("-bad.example"), public_address::kind_e::invalid);
  EXPECT_EQ(public_address::classify("bad-.example"), public_address::kind_e::invalid);
  EXPECT_EQ(public_address::classify("a..b"), public_address::kind_e::invalid);
  EXPECT_EQ(public_address::classify("example/path"), public_address::kind_e::invalid);
  EXPECT_FALSE(public_address::is_valid("not_a_host"));
}

TEST(PublicAddressSyntax, NormalizesWhitespaceAndRootDot) {
  EXPECT_EQ(public_address::normalize("  example.ddns.net. "), "example.ddns.net");
  EXPECT_EQ(public_address::normalize("203.0.113.9"), "203.0.113.9");
}

TEST(NetworkReachabilityConfiguredAddress, UnsetCarriesTheUiHint) {
  const auto result = evaluate_configured_address("", {}, "203.0.113.9");
  EXPECT_EQ(result.kind, public_address::kind_e::empty);
  EXPECT_TRUE(result.value.empty());
  EXPECT_TRUE(has_reason(result.reasons, "unset"));
  EXPECT_EQ(result.hint, "未设置公网地址时客户端使用自动发现的地址");
  EXPECT_FALSE(result.matches_wan);
}

TEST(NetworkReachabilityConfiguredAddress, InvalidValueIsTreatedAsUnset) {
  const auto result = evaluate_configured_address("123.456.789.12", {}, "203.0.113.9");
  EXPECT_EQ(result.kind, public_address::kind_e::empty);
  EXPECT_TRUE(has_reason(result.reasons, "unset"));
  EXPECT_EQ(result.hint, "未设置公网地址时客户端使用自动发现的地址");
}

TEST(NetworkReachabilityConfiguredAddress, PublicIpMatchingWan) {
  const auto result = evaluate_configured_address("203.0.113.9", {}, "203.0.113.9");
  EXPECT_EQ(result.kind, public_address::kind_e::ip);
  EXPECT_TRUE(result.matches_wan);
  EXPECT_FALSE(result.is_private);
  EXPECT_TRUE(result.reasons.empty());
  EXPECT_TRUE(result.hint.empty());
}

TEST(NetworkReachabilityConfiguredAddress, PublicIpDifferentFromWan) {
  const auto result = evaluate_configured_address("198.51.100.7", {}, "203.0.113.9");
  EXPECT_EQ(result.kind, public_address::kind_e::ip);
  EXPECT_FALSE(result.matches_wan);
  EXPECT_TRUE(has_reason(result.reasons, "address_mismatch"));
}

TEST(NetworkReachabilityConfiguredAddress, PrivateIpIsRejected) {
  const auto result = evaluate_configured_address("10.0.1.10", {}, "203.0.113.9");
  EXPECT_TRUE(result.is_private);
  EXPECT_TRUE(has_reason(result.reasons, "address_private"));
  EXPECT_FALSE(has_reason(result.reasons, "address_mismatch"));
}

TEST(NetworkReachabilityConfiguredAddress, HostnameResolvesToWan) {
  const auto result = evaluate_configured_address("example.ddns.net", { "203.0.113.9" }, "203.0.113.9");
  EXPECT_EQ(result.kind, public_address::kind_e::hostname);
  ASSERT_EQ(result.resolved.size(), 1U);
  EXPECT_EQ(result.resolved.front(), "203.0.113.9");
  EXPECT_TRUE(result.matches_wan);
  EXPECT_TRUE(result.reasons.empty());
}

TEST(NetworkReachabilityConfiguredAddress, HostnameResolvesElsewhere) {
  const auto result = evaluate_configured_address("example.ddns.net", { "198.51.100.7" }, "203.0.113.9");
  EXPECT_FALSE(result.matches_wan);
  EXPECT_TRUE(has_reason(result.reasons, "address_mismatch"));
}

TEST(NetworkReachabilityConfiguredAddress, HostnameDnsFailure) {
  const auto result = evaluate_configured_address("example.ddns.net", {}, "203.0.113.9");
  EXPECT_TRUE(result.resolved.empty());
  EXPECT_TRUE(has_reason(result.reasons, "dns_failed"));
}

TEST(NetworkReachabilityConfiguredAddress, HostnameResolvingOnlyToPrivateAddresses) {
  const auto result = evaluate_configured_address("example.lan", { "10.0.1.10", "fd00::10" }, "203.0.113.9");
  EXPECT_TRUE(result.is_private);
  EXPECT_TRUE(has_reason(result.reasons, "address_private"));
  EXPECT_FALSE(has_reason(result.reasons, "address_mismatch"));
}

TEST(NetworkReachabilityPublic, PrivateConfiguredAddressMakesTheVerdictUnreachable) {
  public_inputs_t inputs;
  inputs.ports = healthy_ports();
  inputs.configured = evaluate_configured_address("10.0.1.10", {}, "");
  const auto result = evaluate_public(inputs, NOW);
  EXPECT_EQ(result.verdict, verdict_e::unreachable);
  EXPECT_TRUE(has_reason(result.reasons, "address_private"));
}

TEST(NetworkReachabilityPublic, MismatchedConfiguredAddressLeavesTheVerdictUnknown) {
  public_inputs_t inputs;
  inputs.ports = healthy_ports();
  inputs.configured = evaluate_configured_address("198.51.100.7", {}, "203.0.113.9");
  const auto result = evaluate_public(inputs, NOW);
  EXPECT_EQ(result.verdict, verdict_e::unknown);
  EXPECT_TRUE(has_reason(result.reasons, "address_mismatch"));
}

TEST(NetworkReachabilityPublic, HairpinThroughTheConfiguredAddressVerifies) {
  public_inputs_t inputs;
  inputs.ports = healthy_ports();
  inputs.configured = evaluate_configured_address("198.51.100.7", {}, "203.0.113.9");
  inputs.hairpin = { true, true, 47984, true, "198.51.100.7" };
  const auto result = evaluate_public(inputs, NOW);
  EXPECT_EQ(result.verdict, verdict_e::verified);
  EXPECT_TRUE(has_reason(result.reasons, "hairpin_verified"));
}

TEST(NetworkReachabilityPublic, ConfiguredAddressWithoutUpnpVerifiesByHairpin) {
  public_inputs_t inputs;
  inputs.ports = healthy_ports();
  inputs.configured = evaluate_configured_address("203.0.113.9", {}, "");
  inputs.hairpin = { true, true, 47984, true, "203.0.113.9" };
  const auto result = evaluate_public(inputs, NOW);
  EXPECT_EQ(result.verdict, verdict_e::verified);
  EXPECT_TRUE(has_reason(result.reasons, "hairpin_verified"));
}

// ---- public endpoints ------------------------------------------------------------------------------

namespace {
  using public_address::endpoint_error_e;
  using public_address::kind_e;

  public_address::endpoint_t
  endpoint_of(const std::string &text) {
    const auto parsed = public_address::parse_endpoint(text);
    EXPECT_TRUE(parsed.ok()) << text;
    return parsed.endpoint;
  }

  /// A prober that answers from tables and records what it was asked.
  struct fake_network_t {
    std::map<std::string, std::vector<std::string>> dns;
    std::map<std::string, handshake_t> tls;  // key: "address|port"
    std::mutex mutex;
    std::vector<std::string> asked;

    endpoint_prober_t
    prober() {
      endpoint_prober_t result;
      result.resolve = [this](const std::string &host) {
        std::lock_guard lock { mutex };
        asked.push_back("dns:" + host);
        const auto found = dns.find(host);
        return found == dns.end() ? std::vector<std::string> {} : found->second;
      };
      result.handshake = [this](const std::string &address, std::uint16_t port) {
        std::lock_guard lock { mutex };
        const auto key = address + "|" + std::to_string(port);
        asked.push_back("tls:" + key);
        const auto found = tls.find(key);
        if (found != tls.end()) return found->second;
        handshake_t timeout;
        timeout.error = "timeout";
        timeout.latency_ms = 2500;
        return timeout;
      };
      return result;
    }
  };

  handshake_t
  ours(std::int64_t latency) {
    handshake_t result;
    result.connected = true;
    result.identity_match = true;
    result.latency_ms = latency;
    return result;
  }

  handshake_t
  failed(const std::string &error, std::int64_t latency = 3) {
    handshake_t result;
    result.error = error;
    result.latency_ms = latency;
    return result;
  }
}  // namespace

TEST(PublicEndpointSyntax, AcceptsEveryMoonlightForm) {
  const auto host = endpoint_of("Home.Example.NET.");
  EXPECT_EQ(host.host, "home.example.net");
  EXPECT_EQ(host.port, 0);
  EXPECT_EQ(host.kind, kind_e::hostname);

  const auto host_port = endpoint_of("home.example.net:60000");
  EXPECT_EQ(host_port.host, "home.example.net");
  EXPECT_EQ(host_port.port, 60000);

  const auto v4 = endpoint_of("203.0.113.9");
  EXPECT_EQ(v4.kind, kind_e::ip);
  EXPECT_EQ(v4.port, 0);
  const auto v4_port = endpoint_of(" 203.0.113.9:57989 ");
  EXPECT_EQ(v4_port.host, "203.0.113.9");
  EXPECT_EQ(v4_port.port, 57989);

  const auto v6_port = endpoint_of("[2001:DB8::1]:57989");
  EXPECT_EQ(v6_port.host, "2001:db8::1");
  EXPECT_EQ(v6_port.port, 57989);
  EXPECT_EQ(v6_port.kind, kind_e::ip);
  const auto v6_bracketed = endpoint_of("[2001:db8::1]");
  EXPECT_EQ(v6_bracketed.port, 0);
  const auto v6_bare = endpoint_of("2001:db8::1");
  EXPECT_EQ(v6_bare.host, "2001:db8::1");
  EXPECT_EQ(v6_bare.port, 0);

  EXPECT_EQ(endpoint_of("nas").kind, kind_e::hostname);
  EXPECT_EQ(endpoint_of("100.101.102.103:57989").port, 57989);
}

TEST(PublicEndpointSyntax, RejectsBadInputWithAReason) {
  const auto error_of = [](const char *text) { return public_address::parse_endpoint(text).error; };
  EXPECT_EQ(error_of(""), endpoint_error_e::empty);
  EXPECT_EQ(error_of("   "), endpoint_error_e::empty);
  EXPECT_EQ(error_of("https://home.example.net"), endpoint_error_e::scheme);
  EXPECT_EQ(error_of("home.example.net/path"), endpoint_error_e::format);
  EXPECT_EQ(error_of("user@home.example.net"), endpoint_error_e::format);
  EXPECT_EQ(error_of("home example.net"), endpoint_error_e::format);
  EXPECT_EQ(error_of("home.example.net:"), endpoint_error_e::port);
  EXPECT_EQ(error_of("home.example.net:abc"), endpoint_error_e::port);
  EXPECT_EQ(error_of("home.example.net:0"), endpoint_error_e::port);
  EXPECT_EQ(error_of("home.example.net:5"), endpoint_error_e::port);  // the HTTPS port would be 0
  EXPECT_EQ(error_of("home.example.net:65536"), endpoint_error_e::port);
  EXPECT_EQ(error_of("home.example.net:-1"), endpoint_error_e::port);
  EXPECT_EQ(error_of("203.0.113.9:99999"), endpoint_error_e::port);
  EXPECT_EQ(error_of("256.1.1.1"), endpoint_error_e::host);
  EXPECT_EQ(error_of("123.456.789.12:57989"), endpoint_error_e::host);
  EXPECT_EQ(error_of("-bad.example.net"), endpoint_error_e::host);
  EXPECT_EQ(error_of("bad_name.example.net"), endpoint_error_e::host);
  EXPECT_EQ(error_of("[2001:db8::1"), endpoint_error_e::format);
  EXPECT_EQ(error_of("[2001:db8::1]57989"), endpoint_error_e::format);
  EXPECT_EQ(error_of("[2001:db8::1]:"), endpoint_error_e::port);
  EXPECT_EQ(error_of("[example.com]:57989"), endpoint_error_e::host);
  EXPECT_EQ(error_of("[203.0.113.9]:57989"), endpoint_error_e::host);  // brackets are for IPv6 only
  EXPECT_EQ(error_of("2001:db8::zz"), endpoint_error_e::host);
  EXPECT_EQ(error_of("fe80::1%eth0"), endpoint_error_e::host);  // no zone identifiers
  EXPECT_EQ(error_of("a.b:1:2"), endpoint_error_e::host);
}

TEST(PublicEndpointSyntax, FormatsAndDerivesPorts) {
  EXPECT_EQ(public_address::format_endpoint(endpoint_of("home.example.net")), "home.example.net");
  EXPECT_EQ(public_address::format_endpoint(endpoint_of("[2001:db8::1]:57989")), "[2001:db8::1]:57989");
  EXPECT_EQ(public_address::format_endpoint(endpoint_of("2001:db8::1")), "[2001:db8::1]");
  EXPECT_EQ(public_address::http_port_of(endpoint_of("a.example.net"), 47989), 47989);
  EXPECT_EQ(public_address::http_port_of(endpoint_of("a.example.net:60000"), 47989), 60000);
  EXPECT_EQ(public_address::https_port_of(60000), 59995);
  EXPECT_EQ(public_address::https_port_of(57989), 57984);
  EXPECT_EQ(public_address::https_port_of(5), 0);
}

TEST(PublicEndpointList, SplitsOnCommasSpacesAndLines) {
  const auto list = public_address::parse_endpoint_list("home.example.net:57989, [2001:db8::1]:57989;203.0.113.9\n100.101.102.103 ");
  EXPECT_TRUE(list.rejected.empty());
  ASSERT_EQ(list.endpoints.size(), 4U);
  EXPECT_EQ(public_address::join_endpoints(list.endpoints), "home.example.net:57989, [2001:db8::1]:57989, 203.0.113.9, 100.101.102.103");
}

TEST(PublicEndpointList, AcceptsOneBracketedList) {
  const auto list = public_address::parse_endpoint_list("[home.example.net, [2001:db8::1]:57989]");
  EXPECT_TRUE(list.rejected.empty());
  ASSERT_EQ(list.endpoints.size(), 2U);
  EXPECT_EQ(list.endpoints[1].host, "2001:db8::1");
  // A lone IPv6 literal in brackets is an endpoint, not a list.
  const auto single = public_address::parse_endpoint_list("[2001:db8::1]");
  ASSERT_EQ(single.endpoints.size(), 1U);
  EXPECT_EQ(single.endpoints[0].host, "2001:db8::1");
}

TEST(PublicEndpointList, DropsDuplicatesAndKeepsRejectedAside) {
  const auto list = public_address::parse_endpoint_list("a.example.net, A.EXAMPLE.NET., a.example.net:57989, bad:port, a.example.net:57989");
  ASSERT_EQ(list.endpoints.size(), 2U);  // the same host with and without a port are two endpoints
  ASSERT_EQ(list.rejected.size(), 1U);
  EXPECT_EQ(list.rejected[0], "bad:port");
  EXPECT_FALSE(list.truncated);
}

TEST(PublicEndpointList, KeepsAtMostEight) {
  std::string text;
  for (int index = 1; index <= 10; ++index) text += "h" + std::to_string(index) + ".example.net, ";
  const auto list = public_address::parse_endpoint_list(text);
  EXPECT_EQ(list.endpoints.size(), public_address::MAX_ENDPOINTS);
  EXPECT_TRUE(list.truncated);
  EXPECT_TRUE(public_address::parse_endpoint_list("").endpoints.empty());
}

TEST(PublicEndpointMigration, LegacyExternalIpBecomesTheOnlyEndpoint) {
  const auto list = public_address::effective_endpoints("", "home.example.net");
  ASSERT_EQ(list.endpoints.size(), 1U);
  EXPECT_EQ(list.endpoints[0].host, "home.example.net");
  EXPECT_EQ(list.endpoints[0].port, 0);
}

TEST(PublicEndpointMigration, EndpointsWinOverTheLegacyValue) {
  const auto list = public_address::effective_endpoints("a.example.net:60000, 203.0.113.9", "home.example.net");
  ASSERT_EQ(list.endpoints.size(), 2U);
  EXPECT_EQ(list.endpoints[0].host, "a.example.net");
  EXPECT_EQ(list.endpoints[0].port, 60000);
}

TEST(PublicEndpointMigration, UnusableEndpointsFallBackToTheLegacyValueAndKeepTheRejects) {
  const auto list = public_address::effective_endpoints("bad:port", "203.0.113.9");
  ASSERT_EQ(list.endpoints.size(), 1U);
  EXPECT_EQ(list.endpoints[0].host, "203.0.113.9");
  EXPECT_FALSE(list.rejected.empty());
  EXPECT_TRUE(public_address::effective_endpoints("", "").endpoints.empty());
  EXPECT_TRUE(public_address::effective_endpoints("", "123.456.789.12").endpoints.empty());
}

TEST(NetworkReachabilityScope, SeparatesPublicLanMeshAndLoopback) {
  EXPECT_EQ(scope_of("203.0.113.9"), scope_e::public_address);
  EXPECT_EQ(scope_of("2606:4700:4700::1111"), scope_e::public_address);
  EXPECT_EQ(scope_of("10.0.1.20"), scope_e::lan);
  EXPECT_EQ(scope_of("fd00::1"), scope_e::lan);
  EXPECT_EQ(scope_of("169.254.1.1"), scope_e::lan);
  EXPECT_EQ(scope_of("100.101.102.103"), scope_e::mesh);
  EXPECT_EQ(scope_of("fd7a:115c:a1e0::1"), scope_e::mesh);
  EXPECT_EQ(scope_of("127.0.0.1"), scope_e::loopback);
  EXPECT_EQ(scope_of("::1"), scope_e::loopback);
  EXPECT_EQ(scope_of("0.0.0.0"), scope_e::unknown);
  EXPECT_EQ(scope_of("not-an-address"), scope_e::unknown);
  EXPECT_EQ(scope_of_all({ "10.0.1.20", "203.0.113.9" }), scope_e::public_address);
  EXPECT_EQ(scope_of_all({ "10.0.1.20", "100.101.102.103" }), scope_e::mesh);
}

TEST(NetworkReachabilityEndpoints, ReachedThroughTheHttpsPortDerivedFromTheGivenPort) {
  fake_network_t network;
  network.tls["203.0.113.9|59995"] = ours(14);
  const auto results = probe_endpoints({ endpoint_of("203.0.113.9:60000") }, 57989, "203.0.113.9", network.prober());
  ASSERT_EQ(results.size(), 1U);
  const auto &result = results[0];
  EXPECT_TRUE(result.reached);
  EXPECT_EQ(result.input, "203.0.113.9:60000");
  EXPECT_EQ(result.port, 60000);
  EXPECT_TRUE(result.port_explicit);
  EXPECT_EQ(result.https_port, 59995);
  EXPECT_EQ(result.tested_address, "203.0.113.9");
  EXPECT_EQ(result.latency_ms, 14);
  EXPECT_EQ(result.scope, scope_e::public_address);
  EXPECT_TRUE(result.matches_wan);
  EXPECT_TRUE(result.error.empty());
  ASSERT_EQ(network.asked.size(), 1U);
  EXPECT_EQ(network.asked[0], "tls:203.0.113.9|59995");
}

TEST(NetworkReachabilityEndpoints, AnEndpointWithoutAPortUsesTheConfiguredBasePort) {
  fake_network_t network;
  network.tls["203.0.113.9|57984"] = ours(9);
  const auto results = probe_endpoints({ endpoint_of("203.0.113.9") }, 57989, "203.0.113.9", network.prober());
  ASSERT_EQ(results.size(), 1U);
  EXPECT_TRUE(results[0].reached);
  EXPECT_FALSE(results[0].port_explicit);
  EXPECT_EQ(results[0].port, 57989);
  EXPECT_EQ(results[0].https_port, 57984);
}

TEST(NetworkReachabilityEndpoints, ExternalAndInternalPortsNeedNotMatch) {
  // The router maps external 60000 to internal 57989: the check dials what a client dials, 59995.
  fake_network_t network;
  network.tls["203.0.113.9|59995"] = ours(20);
  network.tls["203.0.113.9|57984"] = failed("refused");
  const auto results = probe_endpoints({ endpoint_of("203.0.113.9:60000"), endpoint_of("203.0.113.9:57989") }, 57989, "203.0.113.9", network.prober());
  ASSERT_EQ(results.size(), 2U);
  EXPECT_TRUE(results[0].reached);
  EXPECT_FALSE(results[1].reached);
  EXPECT_EQ(results[1].error, "refused");
}

TEST(NetworkReachabilityEndpoints, ADdnsNameWithTwoAddressesReportsBothAndReachesThroughEither) {
  fake_network_t network;
  network.dns["home.example.net"] = { "203.0.113.9", "2001:db8::1", "203.0.113.9" };
  network.tls["203.0.113.9|57984"] = failed("timeout", 2500);
  network.tls["2001:db8::1|57984"] = ours(31);
  const auto results = probe_endpoints({ endpoint_of("home.example.net") }, 57989, "203.0.113.9", network.prober());
  ASSERT_EQ(results.size(), 1U);
  const auto &result = results[0];
  ASSERT_EQ(result.resolved.size(), 2U);
  EXPECT_EQ(result.resolved[0], "203.0.113.9");
  EXPECT_EQ(result.resolved[1], "2001:db8::1");
  EXPECT_TRUE(result.reached);
  EXPECT_EQ(result.tested_address, "2001:db8::1");
  EXPECT_EQ(result.latency_ms, 31);
  ASSERT_EQ(result.attempts.size(), 2U);
  EXPECT_TRUE(result.matches_wan);
}

TEST(NetworkReachabilityEndpoints, FailureReasonsAreSpecific) {
  fake_network_t network;
  network.dns["gone.example.net"] = {};
  network.dns["closed.example.net"] = { "203.0.113.20" };
  network.dns["silent.example.net"] = { "203.0.113.21" };
  network.dns["other.example.net"] = { "203.0.113.22" };
  network.dns["tls.example.net"] = { "203.0.113.23" };
  network.tls["203.0.113.20|57984"] = failed("refused");
  network.tls["203.0.113.21|57984"] = failed("timeout", 2500);
  handshake_t other;
  other.connected = true;
  other.identity_match = false;
  other.latency_ms = 40;
  network.tls["203.0.113.22|57984"] = other;
  network.tls["203.0.113.23|57984"] = failed("tls_failed");
  const auto results = probe_endpoints({ endpoint_of("gone.example.net"), endpoint_of("closed.example.net"), endpoint_of("silent.example.net"), endpoint_of("other.example.net"), endpoint_of("tls.example.net") },
    57989, "203.0.113.9", network.prober());
  ASSERT_EQ(results.size(), 5U);
  EXPECT_EQ(results[0].error, "dns_failed");
  EXPECT_TRUE(results[0].attempts.empty());
  EXPECT_EQ(results[1].error, "refused");
  EXPECT_EQ(results[2].error, "timeout");
  EXPECT_EQ(results[3].error, "identity_mismatch");
  EXPECT_EQ(results[4].error, "tls_failed");
  for (const auto &result : results) EXPECT_FALSE(result.reached);
  EXPECT_TRUE(has_reason(results[1].reasons, "address_mismatch"));
}

TEST(NetworkReachabilityEndpoints, LanAndMeshAndLoopbackEndpointsAreLabelled) {
  fake_network_t network;
  network.tls["10.0.1.10|57984"] = ours(1);
  network.tls["100.101.102.103|57984"] = ours(7);
  network.tls["127.0.0.1|57984"] = ours(1);
  const auto results = probe_endpoints({ endpoint_of("10.0.1.10"), endpoint_of("100.101.102.103"), endpoint_of("127.0.0.1") }, 57989, "203.0.113.9", network.prober());
  ASSERT_EQ(results.size(), 3U);
  EXPECT_EQ(results[0].scope, scope_e::lan);
  EXPECT_TRUE(has_reason(results[0].reasons, "scope_lan"));
  EXPECT_EQ(results[1].scope, scope_e::mesh);
  EXPECT_TRUE(has_reason(results[1].reasons, "scope_mesh"));
  EXPECT_EQ(results[2].scope, scope_e::loopback);
  EXPECT_FALSE(has_reason(results[0].reasons, "address_mismatch"));
  for (const auto &result : results) EXPECT_TRUE(result.reached);
}

TEST(NetworkReachabilityEndpoints, AnAddressOfAnotherFamilyIsNotAMismatch) {
  fake_network_t network;
  network.tls["2001:db8::1|57984"] = ours(5);
  const auto results = probe_endpoints({ endpoint_of("[2001:db8::1]") }, 57989, "203.0.113.9", network.prober());
  ASSERT_EQ(results.size(), 1U);
  EXPECT_FALSE(has_reason(results[0].reasons, "address_mismatch"));
}

TEST(NetworkReachabilityEndpoints, EveryEndpointIsProbedAtMostEightAreKept) {
  fake_network_t network;
  std::vector<public_address::endpoint_t> endpoints;
  for (int index = 1; index <= 12; ++index) endpoints.push_back(endpoint_of("198.51.100." + std::to_string(index)));
  const auto results = probe_endpoints(endpoints, 57989, "203.0.113.9", network.prober());
  EXPECT_EQ(results.size(), public_address::MAX_ENDPOINTS);
  EXPECT_EQ(network.asked.size(), public_address::MAX_ENDPOINTS);
}

TEST(NetworkReachabilityEndpoints, ARealPortOfFiveOrBelowIsNotTried) {
  fake_network_t network;
  public_address::endpoint_t endpoint;
  endpoint.host = "203.0.113.9";
  endpoint.kind = kind_e::ip;
  endpoint.port = 3;
  const auto results = probe_endpoints({ endpoint }, 57989, "203.0.113.9", network.prober());
  ASSERT_EQ(results.size(), 1U);
  EXPECT_EQ(results[0].error, "not_tried");
  EXPECT_TRUE(network.asked.empty());
}

TEST(NetworkReachabilityPublic, AReachedPublicEndpointVerifiesWithoutAnyRouterAnswer) {
  fake_network_t network;
  network.tls["203.0.113.9|59995"] = ours(10);
  public_inputs_t inputs;
  inputs.ports = healthy_ports();
  inputs.endpoints = probe_endpoints({ endpoint_of("203.0.113.9:60000") }, 47989, "", network.prober());
  const auto result = evaluate_public(inputs, NOW);
  EXPECT_EQ(result.verdict, verdict_e::verified);
  EXPECT_TRUE(has_reason(result.reasons, "hairpin_verified"));
  ASSERT_EQ(result.endpoints.size(), 1U);
}

TEST(NetworkReachabilityPublic, OneReachedEndpointOfSeveralIsEnough) {
  fake_network_t network;
  network.dns["gone.example.net"] = {};
  network.tls["198.51.100.7|47984"] = ours(11);
  public_inputs_t inputs;
  inputs.ports = healthy_ports();
  inputs.endpoints = probe_endpoints({ endpoint_of("gone.example.net"), endpoint_of("203.0.113.9"), endpoint_of("198.51.100.7") }, 47989, "", network.prober());
  EXPECT_EQ(evaluate_public(inputs, NOW).verdict, verdict_e::verified);
}

TEST(NetworkReachabilityPublic, OnlyLanEndpointsAreUnreachableFromOutside) {
  fake_network_t network;
  network.tls["10.0.1.10|47984"] = ours(1);
  public_inputs_t inputs;
  inputs.ports = healthy_ports();
  inputs.endpoints = probe_endpoints({ endpoint_of("10.0.1.10") }, 47989, "203.0.113.9", network.prober());
  const auto result = evaluate_public(inputs, NOW);
  EXPECT_EQ(result.verdict, verdict_e::unreachable);
  EXPECT_TRUE(has_reason(result.reasons, "address_private"));
}

TEST(NetworkReachabilityPublic, AMeshEndpointAloneDoesNotCondemnThePublicPath) {
  fake_network_t network;
  network.tls["100.101.102.103|47984"] = ours(4);
  public_inputs_t inputs;
  inputs.ports = healthy_ports();
  inputs.endpoints = probe_endpoints({ endpoint_of("100.101.102.103") }, 47989, "", network.prober());
  EXPECT_EQ(evaluate_public(inputs, NOW).verdict, verdict_e::unknown);
}

TEST(NetworkReachabilityPublic, NamesThatDoNotResolveLeaveTheVerdictUnknown) {
  fake_network_t network;
  public_inputs_t inputs;
  inputs.ports = healthy_ports();
  inputs.endpoints = probe_endpoints({ endpoint_of("gone.example.net") }, 47989, "203.0.113.9", network.prober());
  const auto result = evaluate_public(inputs, NOW);
  EXPECT_EQ(result.verdict, verdict_e::unknown);
  EXPECT_TRUE(has_reason(result.reasons, "dns_failed"));
}

TEST(NetworkReachabilityPublic, PublicEndpointsThatAreNotTheRouterAddressAreUnconfirmed) {
  fake_network_t network;
  network.tls["198.51.100.7|47984"] = failed("timeout", 2500);
  public_inputs_t inputs;
  inputs.ports = healthy_ports();
  inputs.router = public_router(true);
  inputs.endpoints = probe_endpoints({ endpoint_of("198.51.100.7") }, 47989, "203.0.113.9", network.prober());
  const auto result = evaluate_public(inputs, NOW);
  EXPECT_EQ(result.verdict, verdict_e::unknown);
  EXPECT_TRUE(has_reason(result.reasons, "address_mismatch"));
}

TEST(NetworkReachabilityPublic, AnEndpointThatTimesOutFallsBackToTheRouterEvidence) {
  fake_network_t network;
  network.tls["203.0.113.9|47984"] = failed("timeout", 2500);
  public_inputs_t inputs;
  inputs.ports = healthy_ports();
  inputs.router = public_router(true);
  inputs.endpoints = probe_endpoints({ endpoint_of("203.0.113.9") }, 47989, "203.0.113.9", network.prober());
  inputs.hairpin = { true, false, 47984, false, "203.0.113.9" };
  const auto result = evaluate_public(inputs, NOW);
  EXPECT_EQ(result.verdict, verdict_e::likely);
  EXPECT_TRUE(has_reason(result.reasons, "mappings_complete"));
}

TEST(NetworkReachabilityJson, EndpointsCarryEveryFieldTheUiShows) {
  fake_network_t network;
  network.dns["home.example.net"] = { "203.0.113.9", "2001:db8::1" };
  network.tls["203.0.113.9|59995"] = ours(14);
  network.tls["2001:db8::1|59995"] = failed("refused", 4);
  snapshot_t snapshot;
  snapshot.checked_at = format_utc_iso(NOW);
  snapshot.ports = healthy_ports();
  for (auto &port : snapshot.ports) {
    if (port.proto == "udp") port.listening = false;
  }
  public_inputs_t inputs;
  inputs.ports = snapshot.ports;
  inputs.endpoints = probe_endpoints({ endpoint_of("home.example.net:60000"), endpoint_of("gone.example.net") }, 47989, "203.0.113.9", network.prober());
  snapshot.public_reachability = evaluate_public(inputs, NOW);
  const auto json = to_json(snapshot);

  const auto &endpoints = json.at("public").at("endpoints");
  ASSERT_EQ(endpoints.size(), 2U);
  const auto &first = endpoints.at(0);
  EXPECT_EQ(first.at("input"), "home.example.net:60000");
  EXPECT_EQ(first.at("host"), "home.example.net");
  EXPECT_EQ(first.at("kind"), "hostname");
  EXPECT_EQ(first.at("port"), 60000);
  EXPECT_EQ(first.at("port_explicit"), true);
  EXPECT_EQ(first.at("https_port"), 59995);
  EXPECT_EQ(first.at("resolved").size(), 2U);
  EXPECT_EQ(first.at("scope"), "public");
  EXPECT_EQ(first.at("state"), "reached");
  EXPECT_TRUE(first.at("error").is_null());
  EXPECT_EQ(first.at("tested").at("address"), "203.0.113.9");
  EXPECT_EQ(first.at("tested").at("port"), 59995);
  EXPECT_EQ(first.at("tested").at("latency_ms"), 14);
  ASSERT_EQ(first.at("attempts").size(), 2U);
  EXPECT_EQ(first.at("attempts").at(1).at("error"), "refused");
  const auto &second = endpoints.at(1);
  EXPECT_EQ(second.at("state"), "failed");
  EXPECT_EQ(second.at("error"), "dns_failed");
  EXPECT_TRUE(second.at("tested").is_null());
  EXPECT_EQ(second.at("port"), 47989);
  EXPECT_EQ(second.at("port_explicit"), false);
  EXPECT_EQ(second.at("https_port"), 47984);
  // The ports table says which sockets exist only while a session runs.
  EXPECT_EQ(json.at("ports").at(1).at("on_demand"), false);
  EXPECT_EQ(json.at("ports").at(4).at("role"), "video");
  EXPECT_EQ(json.at("ports").at(4).at("on_demand"), true);
}

// The one-off address test of GET /api/network/reachability?test=...: what reaches the probe is only what parses.
TEST(ReachabilityTestEndpoints, KeepsValidEntriesInTheCanonicalListAndDropsTheRest) {
  EXPECT_EQ(test_endpoints_option("Home.Example.net"), "home.example.net");
  EXPECT_EQ(test_endpoints_option("home.example.net:57989, 203.0.113.7"), "home.example.net:57989, 203.0.113.7");
  EXPECT_EQ(test_endpoints_option("home.example.net, home.example.net"), "home.example.net");
  EXPECT_EQ(test_endpoints_option("[2001:db8::1]:57989"), "[2001:db8::1]:57989");
  EXPECT_EQ(test_endpoints_option("http://home.example.net"), "");
  EXPECT_EQ(test_endpoints_option(""), "");
}

TEST(ReachabilityTestEndpoints, IsBoundedBeforeParsingAndKeepsAtMostTheLimit) {
  EXPECT_EQ(test_endpoints_option(std::string(5000, 'a')), "");
  std::string many;
  for (int i = 1; i <= 20; ++i) many += "h" + std::to_string(i) + ".example.net,";
  const auto kept = test_endpoints_option(many);
  EXPECT_EQ(static_cast<std::size_t>(std::count(kept.begin(), kept.end(), ',')) + 1, public_address::MAX_ENDPOINTS);
}
