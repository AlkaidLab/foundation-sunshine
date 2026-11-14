/**
 * @file tests/unit/test_nat_discovery.cpp
 * @brief STUN parsing, the RFC 5780 mapping decision, address classes and the overview contract.
 */
#include <cstring>

#include <alkaidlab/network/stun_codec.h>
#include <gtest/gtest.h>

#include "src/nat_discovery.h"

using namespace nat_discovery;

namespace {
  // RFC 5769 section 2.2 and 2.3 sample responses (with SOFTWARE, MESSAGE-INTEGRITY and FINGERPRINT).
  const std::uint8_t RFC5769_IPV4[] = {
    0x01, 0x01, 0x00, 0x3c, 0x21, 0x12, 0xa4, 0x42, 0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34, 0xd6, 0x86,
    0xfa, 0x87, 0xdf, 0xae, 0x80, 0x22, 0x00, 0x0b, 0x74, 0x65, 0x73, 0x74, 0x20, 0x76, 0x65, 0x63,
    0x74, 0x6f, 0x72, 0x20, 0x00, 0x20, 0x00, 0x08, 0x00, 0x01, 0xa1, 0x47, 0xe1, 0x12, 0xa6, 0x43,
    0x00, 0x08, 0x00, 0x14, 0x2b, 0x91, 0xf5, 0x99, 0xfd, 0x9e, 0x90, 0xc3, 0x8c, 0x74, 0x89, 0xf9,
    0x2a, 0xf9, 0xba, 0x53, 0xf0, 0x6b, 0xe7, 0xd7, 0x80, 0x28, 0x00, 0x04, 0xc0, 0x7d, 0x4c, 0x96,
  };
  const std::uint8_t RFC5769_IPV6[] = {
    0x01, 0x01, 0x00, 0x48, 0x21, 0x12, 0xa4, 0x42, 0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34, 0xd6, 0x86, 0xfa, 0x87, 0xdf,
    0xae, 0x80, 0x22, 0x00, 0x0b, 0x74, 0x65, 0x73, 0x74, 0x20, 0x76, 0x65, 0x63, 0x74, 0x6f, 0x72, 0x20, 0x00, 0x20,
    0x00, 0x14, 0x00, 0x02, 0xa1, 0x47, 0x01, 0x13, 0xa9, 0xfa, 0xa5, 0xd3, 0xf1, 0x79, 0xbc, 0x25, 0xf4, 0xb5, 0xbe,
    0xd2, 0xb9, 0xd9, 0x00, 0x08, 0x00, 0x14, 0xa3, 0x82, 0x95, 0x4e, 0x4b, 0xe6, 0x7b, 0xf1, 0x17, 0x84, 0xc9, 0x7c,
    0x82, 0x92, 0xc2, 0x75, 0xbf, 0xe3, 0xed, 0x41, 0x80, 0x28, 0x00, 0x04, 0xc8, 0xfb, 0x0b, 0x4c,
  };
  const transaction_id_t RFC5769_ID = { 0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34, 0xd6, 0x86, 0xfa, 0x87, 0xdf, 0xae };

  transport_address_t
  ta(std::string address, std::uint16_t port) {
    return { std::move(address), port };
  }

  /// A success response with XOR-MAPPED-ADDRESS and an RFC 5780 OTHER-ADDRESS (plain address encoding).
  std::vector<std::uint8_t>
  response_with_other(const transaction_id_t &id) {
    std::vector<std::uint8_t> storage(128);
    AlkStunMessageBuilder builder;
    EXPECT_TRUE(alk_stun_builder_init(&builder, storage.data(), storage.size(), ALK_STUN_BINDING_SUCCESS_RESPONSE, id.data()));
    AlkStunMappedEndpoint mapped;
    alk_stun_mapped_endpoint_init(&mapped);
    mapped.address_family = ALK_STUN_ADDRESS_IPV4;
    mapped.port = 58409;
    const std::uint8_t mapped_ip[] = { 203, 0, 113, 56 };
    std::memcpy(mapped.address, mapped_ip, 4);
    EXPECT_TRUE(alk_stun_builder_add_xor_mapped_endpoint(&builder, &mapped));
    const std::uint8_t other[] = { 0x00, 0x01, 0x0D, 0x97, 198, 51, 100, 2 };  // 198.51.100.2:3479
    EXPECT_TRUE(alk_stun_builder_add(&builder, ALK_STUN_ATTR_OTHER_ADDRESS, other, sizeof(other)));
    storage.resize(builder.length);
    return storage;
  }

  reachability::snapshot_t
  snapshot_with(reachability::verdict_e verdict, std::vector<std::string> reasons, std::string wan, reachability::ip_class_e wan_class) {
    reachability::snapshot_t snapshot;
    snapshot.public_reachability.verdict = verdict;
    snapshot.public_reachability.reasons = std::move(reasons);
    snapshot.public_reachability.router.protocol = wan.empty() ? "none" : "upnp-igd";
    snapshot.public_reachability.router.wan_ip = std::move(wan);
    snapshot.public_reachability.router.wan_ip_class = wan_class;
    snapshot.ports = {
      { "tcp", 47984, "https", true, { "::" }, reachability::firewall_e::allowed },
      { "tcp", 47989, "http", true, { "::" }, reachability::firewall_e::allowed },
      { "tcp", 48010, "rtsp", true, { "::" }, reachability::firewall_e::allowed },
    };
    return snapshot;
  }

  interface_t
  ethernet(std::vector<interface_address_t> addresses, bool egress = true) {
    interface_t entry;
    entry.name = "eth0";
    entry.up = true;
    entry.egress_v4 = egress;
    entry.egress_v6 = egress;
    entry.addresses = std::move(addresses);
    return entry;
  }

  interface_address_t
  v6(std::string address, lifetime_e lifetime, bool deprecated = false) {
    interface_address_t item;
    item.address = std::move(address);
    item.family = 6;
    item.prefix_length = 64;
    item.scope = classify_address(item.address);
    item.lifetime = lifetime;
    item.deprecated = deprecated;
    return item;
  }
}  // namespace

// ---- STUN messages (RFC 8489) -------------------------------------------------

TEST(NatDiscoveryStun, BindingRequestIsAHeaderOnlyRequest) {
  const transaction_id_t id = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
  const auto request = build_binding_request(id);
  ASSERT_EQ(request.size(), 20u);
  EXPECT_EQ(request[0], 0x00);
  EXPECT_EQ(request[1], 0x01);  // Binding request (section 5, 18.2)
  EXPECT_EQ(request[2], 0x00);
  EXPECT_EQ(request[3], 0x00);  // no attributes
  EXPECT_EQ(request[4], 0x21);
  EXPECT_EQ(request[5], 0x12);
  EXPECT_EQ(request[6], 0xA4);
  EXPECT_EQ(request[7], 0x42);  // magic cookie
  EXPECT_EQ(0, std::memcmp(request.data() + 8, id.data(), id.size()));
}

TEST(NatDiscoveryStun, DecodesTheRfc5769Ipv4Response) {
  const auto response = parse_binding_response(RFC5769_IPV4, sizeof(RFC5769_IPV4), RFC5769_ID);
  ASSERT_TRUE(response);
  EXPECT_TRUE(response->success);
  ASSERT_TRUE(response->mapped);
  EXPECT_EQ(*response->mapped, ta("192.0.2.1", 32853));
  EXPECT_FALSE(response->other);
}

TEST(NatDiscoveryStun, DecodesTheRfc5769Ipv6Response) {
  const auto response = parse_binding_response(RFC5769_IPV6, sizeof(RFC5769_IPV6), RFC5769_ID);
  ASSERT_TRUE(response);
  ASSERT_TRUE(response->mapped);
  EXPECT_EQ(*response->mapped, ta("2001:db8:1234:5678:11:2233:4455:6677", 32853));
}

TEST(NatDiscoveryStun, IgnoresAnotherTransaction) {
  transaction_id_t other = RFC5769_ID;
  other[11] ^= 0xFF;
  EXPECT_FALSE(parse_binding_response(RFC5769_IPV4, sizeof(RFC5769_IPV4), other));
}

TEST(NatDiscoveryStun, RejectsTruncatedAndMalformedDatagrams) {
  EXPECT_FALSE(parse_binding_response(RFC5769_IPV4, 19, RFC5769_ID));
  EXPECT_FALSE(parse_binding_response(RFC5769_IPV4, sizeof(RFC5769_IPV4) - 4, RFC5769_ID));
  std::vector<std::uint8_t> bad_cookie(RFC5769_IPV4, RFC5769_IPV4 + sizeof(RFC5769_IPV4));
  bad_cookie[4] = 0x00;
  EXPECT_FALSE(parse_binding_response(bad_cookie.data(), bad_cookie.size(), RFC5769_ID));
  EXPECT_FALSE(parse_binding_response(nullptr, 0, RFC5769_ID));
}

TEST(NatDiscoveryStun, ReadsOtherAddressOfAnRfc5780Server) {
  const transaction_id_t id = { 9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 1, 2 };
  const auto bytes = response_with_other(id);
  const auto response = parse_binding_response(bytes.data(), bytes.size(), id);
  ASSERT_TRUE(response);
  ASSERT_TRUE(response->mapped);
  EXPECT_EQ(*response->mapped, ta("203.0.113.56", 58409));
  ASSERT_TRUE(response->other);
  EXPECT_EQ(*response->other, ta("198.51.100.2", 3479));
}

TEST(NatDiscoveryStun, ReportsAnErrorResponse) {
  const transaction_id_t id = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
  std::vector<std::uint8_t> storage(64);
  AlkStunMessageBuilder builder;
  ASSERT_TRUE(alk_stun_builder_init(&builder, storage.data(), storage.size(), 0x0111, id.data()));
  const std::uint8_t error[] = { 0x00, 0x00, 0x04, 0x00 };  // class 4, number 0: 400
  ASSERT_TRUE(alk_stun_builder_add(&builder, ALK_STUN_ATTR_ERROR_CODE, error, sizeof(error)));
  const auto response = parse_binding_response(storage.data(), builder.length, id);
  ASSERT_TRUE(response);
  EXPECT_FALSE(response->success);
  EXPECT_EQ(response->error_code, 400);
  EXPECT_FALSE(response->mapped);
}

// ---- RFC 5780 section 4.3 -----------------------------------------------------

TEST(NatDiscoveryMapping, SameAddressAndPortAsTheSocketMeansNoNat) {
  mapping_inputs_t inputs;
  inputs.method = method_e::rfc5780;
  inputs.local = ta("198.51.100.7", 50000);
  inputs.test1 = ta("198.51.100.7", 50000);
  const auto result = classify_mapping(inputs);
  ASSERT_TRUE(result.nat_present);
  EXPECT_FALSE(*result.nat_present);
  EXPECT_EQ(result.mapping, mapping_e::endpoint_independent);
}

TEST(NatDiscoveryMapping, OwnAddressWithAnotherPortIsStillTranslated) {
  mapping_inputs_t inputs;
  inputs.method = method_e::rfc5780;
  inputs.local = ta("198.51.100.7", 50000);
  inputs.test1 = ta("198.51.100.7", 61000);
  inputs.test2 = ta("198.51.100.7", 61000);
  const auto result = classify_mapping(inputs);
  EXPECT_TRUE(result.nat_present.value_or(false));
  EXPECT_EQ(result.mapping, mapping_e::endpoint_independent);
  EXPECT_NE(std::find(result.reasons.begin(), result.reasons.end(), "port_translated"), result.reasons.end());
}

TEST(NatDiscoveryMapping, TestTwoEqualToTestOneIsEndpointIndependent) {
  mapping_inputs_t inputs;
  inputs.method = method_e::rfc5780;
  inputs.local = ta("10.0.1.10", 50000);
  inputs.test1 = ta("203.0.113.56", 58409);
  inputs.test2 = ta("203.0.113.56", 58409);
  inputs.test3_possible = true;
  const auto result = classify_mapping(inputs);
  EXPECT_TRUE(result.nat_present.value_or(false));
  EXPECT_EQ(result.mapping, mapping_e::endpoint_independent);
  EXPECT_FALSE(result.mapped_address_varies);
}

TEST(NatDiscoveryMapping, TestThreeEqualToTestTwoIsAddressDependent) {
  mapping_inputs_t inputs;
  inputs.method = method_e::rfc5780;
  inputs.local = ta("10.0.1.10", 50000);
  inputs.test1 = ta("203.0.113.56", 58409);
  inputs.test2 = ta("203.0.113.56", 58410);
  inputs.test3_possible = true;
  inputs.test3 = ta("203.0.113.56", 58410);
  EXPECT_EQ(classify_mapping(inputs).mapping, mapping_e::address_dependent);
}

TEST(NatDiscoveryMapping, TestThreeDifferentFromTestTwoIsAddressAndPortDependent) {
  mapping_inputs_t inputs;
  inputs.method = method_e::rfc5780;
  inputs.local = ta("10.0.1.10", 50000);
  inputs.test1 = ta("203.0.113.56", 58409);
  inputs.test2 = ta("203.0.113.56", 58410);
  inputs.test3_possible = true;
  inputs.test3 = ta("203.0.113.56", 58411);
  EXPECT_EQ(classify_mapping(inputs).mapping, mapping_e::address_and_port_dependent);
}

TEST(NatDiscoveryMapping, NoAnswerToTestTwoLeavesTheMappingUnknown) {
  mapping_inputs_t inputs;
  inputs.method = method_e::rfc5780;
  inputs.local = ta("10.0.1.10", 50000);
  inputs.test1 = ta("203.0.113.56", 58409);
  const auto result = classify_mapping(inputs);
  EXPECT_EQ(result.mapping, mapping_e::unknown);
  EXPECT_EQ(result.reasons.back(), "test2_no_answer");
}

TEST(NatDiscoveryMapping, NoAnswerAtAllIsUnknownWithoutANatClaim) {
  mapping_inputs_t inputs;
  inputs.method = method_e::rfc5780;
  const auto result = classify_mapping(inputs);
  EXPECT_FALSE(result.nat_present);
  EXPECT_EQ(result.mapping, mapping_e::unknown);
}

TEST(NatDiscoveryMapping, TwoServersCannotSeparateTheDependentKinds) {
  mapping_inputs_t inputs;
  inputs.method = method_e::two_servers;
  inputs.local = ta("10.0.1.10", 50000);
  inputs.test1 = ta("203.0.113.56", 58409);
  inputs.test2 = ta("203.0.113.56", 60001);
  const auto result = classify_mapping(inputs);
  EXPECT_EQ(result.mapping, mapping_e::endpoint_dependent);
  EXPECT_EQ(result.reasons.back(), "test3_not_possible");

  inputs.test2 = inputs.test1;
  EXPECT_EQ(classify_mapping(inputs).mapping, mapping_e::endpoint_independent);
}

TEST(NatDiscoveryMapping, DifferentPublicAddressesAreFlagged) {
  // Split routing or a NAT with arbitrary address pooling (RFC 4787 section 4.1) shows another IP.
  mapping_inputs_t inputs;
  inputs.method = method_e::two_servers;
  inputs.local = ta("10.0.1.10", 50000);
  inputs.test1 = ta("203.0.113.56", 58409);
  inputs.test2 = ta("198.51.100.88", 58409);
  const auto result = classify_mapping(inputs);
  EXPECT_TRUE(result.mapped_address_varies);
  EXPECT_EQ(result.mapping, mapping_e::endpoint_dependent);
}

TEST(NatDiscoveryMapping, OneServerOnlyShowsThatThereIsANat) {
  mapping_inputs_t inputs;
  inputs.method = method_e::none;
  inputs.local = ta("10.0.1.10", 50000);
  inputs.test1 = ta("203.0.113.56", 58409);
  const auto result = classify_mapping(inputs);
  EXPECT_TRUE(result.nat_present.value_or(false));
  EXPECT_EQ(result.mapping, mapping_e::unknown);
}

// ---- addresses ----------------------------------------------------------------

TEST(NatDiscoveryAddress, ClassifiesIpv4) {
  EXPECT_EQ(classify_address("10.0.100.186"), address_scope_e::private_v4);
  EXPECT_EQ(classify_address("10.1.2.3"), address_scope_e::private_v4);
  EXPECT_EQ(classify_address("172.31.0.1"), address_scope_e::private_v4);
  EXPECT_EQ(classify_address("172.32.0.1"), address_scope_e::global);
  EXPECT_EQ(classify_address("100.64.0.1"), address_scope_e::cgnat);
  EXPECT_EQ(classify_address("100.127.255.254"), address_scope_e::cgnat);
  EXPECT_EQ(classify_address("100.128.0.1"), address_scope_e::global);
  EXPECT_EQ(classify_address("169.254.10.1"), address_scope_e::link_local);
  EXPECT_EQ(classify_address("127.0.0.1"), address_scope_e::loopback);
  EXPECT_EQ(classify_address("192.0.2.1"), address_scope_e::other);
  EXPECT_EQ(classify_address("180.165.48.56"), address_scope_e::global);
  EXPECT_EQ(classify_address("not an address"), address_scope_e::other);
}

TEST(NatDiscoveryAddress, ClassifiesIpv6) {
  EXPECT_EQ(classify_address("240e:c2:1800:16:4000::15"), address_scope_e::global);
  EXPECT_EQ(classify_address("2408:8207:1234::1"), address_scope_e::global);
  EXPECT_EQ(classify_address("fd4b:c064:5bfa:48a2:ea90:3588:aec9:7ff6"), address_scope_e::ula);
  EXPECT_EQ(classify_address("fe80::1%eth0"), address_scope_e::link_local);
  EXPECT_EQ(classify_address("2001:db8::1"), address_scope_e::other);
  EXPECT_EQ(classify_address("::1"), address_scope_e::loopback);
  EXPECT_EQ(classify_address("::ffff:10.0.1.2"), address_scope_e::private_v4);
}

TEST(NatDiscoveryAddress, ParsesLinuxIfInet6Rows) {
  const auto stable = parse_if_inet6_line("fd4bc0645bfa48a2ea903588aec97ff6 05 40 00 00   enp6s0");
  ASSERT_TRUE(stable);
  EXPECT_EQ(stable->address, "fd4b:c064:5bfa:48a2:ea90:3588:aec9:7ff6");
  EXPECT_EQ(stable->index, 5u);
  EXPECT_EQ(stable->prefix_length, 64);
  EXPECT_EQ(stable->flags & IFA_TEMPORARY, 0);
  EXPECT_EQ(stable->name, "enp6s0");

  const auto temporary = parse_if_inet6_line("240e03a1286013d01c2a35fffe9c1a2b 0c 40 00 21 eth0");
  ASSERT_TRUE(temporary);
  EXPECT_EQ(temporary->index, 12u);
  EXPECT_NE(temporary->flags & IFA_TEMPORARY, 0);
  EXPECT_NE(temporary->flags & IFA_DEPRECATED, 0);

  EXPECT_FALSE(parse_if_inet6_line(""));
  EXPECT_FALSE(parse_if_inet6_line("zz4bc0645bfa48a2ea903588aec97ff6 05 40 00 00 enp6s0"));
  EXPECT_FALSE(parse_if_inet6_line("fd4b 05 40 00 00 enp6s0"));
}

TEST(NatDiscoveryAddress, WindowsRandomSuffixIsTemporaryOnlyOutsideDns) {
  EXPECT_EQ(lifetime_from_windows(5, 0), lifetime_e::temporary);
  EXPECT_EQ(lifetime_from_windows(5, WINDOWS_DNS_ELIGIBLE), lifetime_e::stable);
  EXPECT_EQ(lifetime_from_windows(4, WINDOWS_DNS_ELIGIBLE), lifetime_e::stable);
  EXPECT_EQ(lifetime_from_windows(3, 0), lifetime_e::stable);
  EXPECT_EQ(lifetime_from_windows(1, 0), lifetime_e::stable);
  EXPECT_EQ(lifetime_from_windows(0, 0), lifetime_e::unknown);
}

TEST(NatDiscoveryAddress, PrefersTheStableGlobalAddress) {
  const std::vector<interface_t> interfaces = {
    ethernet({
      v6("2408:8207:1:2:aaaa:bbbb:cccc:dddd", lifetime_e::temporary),
      v6("2408:8207:1:2::abcd", lifetime_e::stable),
      v6("fd00::5", lifetime_e::stable),
    }),
  };
  EXPECT_EQ(preferred_global(interfaces, 6), "2408:8207:1:2::abcd");
}

TEST(NatDiscoveryAddress, DeprecatedStableLosesToAPreferredStable) {
  const std::vector<interface_t> interfaces = {
    ethernet({
      v6("2408:8207:1:1::1", lifetime_e::stable, true),
      v6("2408:8207:1:2::1", lifetime_e::stable),
    }),
  };
  EXPECT_EQ(preferred_global(interfaces, 6), "2408:8207:1:2::1");
}

TEST(NatDiscoveryAddress, OnlyTemporaryIsStillShown) {
  const std::vector<interface_t> interfaces = { ethernet({ v6("2408:8207:1:2:aaaa:bbbb:cccc:dddd", lifetime_e::temporary) }) };
  EXPECT_EQ(preferred_global(interfaces, 6), "2408:8207:1:2:aaaa:bbbb:cccc:dddd");
}

TEST(NatDiscoveryAddress, MeshAndDownInterfacesAreSkipped) {
  auto mesh = ethernet({ v6("2001:470:1::1", lifetime_e::stable) });
  mesh.mesh = reachability::mesh_kind_e::wireguard;
  auto down = ethernet({ v6("2001:470:2::1", lifetime_e::stable) });
  down.up = false;
  EXPECT_EQ(preferred_global({ mesh, down }, 6), "");
  EXPECT_EQ(preferred_global({ mesh, down, ethernet({ v6("2001:470:3::1", lifetime_e::stable) }, false) }, 6), "2001:470:3::1");
}

TEST(NatDiscoveryAddress, EgressInterfaceWins) {
  const std::vector<interface_t> interfaces = {
    ethernet({ v6("2001:470:1::1", lifetime_e::stable) }, false),
    ethernet({ v6("2001:470:2::1", lifetime_e::temporary) }, true),
  };
  EXPECT_EQ(preferred_global(interfaces, 6), "2001:470:2::1");
}

TEST(NatDiscoveryAddress, ContainerBridgesWithNonPrivateRangesAreNotPublic) {
  // Seen on the Linux build host: Docker networks carved from 11.77.0.0/16, which is not RFC 1918.
  interface_t bridge;
  bridge.name = "br-48e27b925fa5";
  bridge.up = true;
  bridge.is_virtual = is_virtual_adapter(bridge.name);
  interface_address_t address;
  address.address = "11.77.163.1";
  address.family = 4;
  address.scope = classify_address(address.address);
  bridge.addresses = { address };
  EXPECT_TRUE(bridge.is_virtual);
  EXPECT_EQ(address.scope, address_scope_e::global);
  EXPECT_EQ(preferred_global({ bridge }, 4), "");
  EXPECT_TRUE(is_virtual_adapter("vEthernet (Default Switch)"));
  EXPECT_TRUE(is_virtual_adapter("docker0"));
  EXPECT_FALSE(is_virtual_adapter("enp6s0"));
  EXPECT_FALSE(is_virtual_adapter("Ethernet"));
}

TEST(NatDiscoveryAddress, RecognizesFakeIpAnswers) {
  EXPECT_TRUE(is_fake_ip("198.19.33.102"));
  EXPECT_TRUE(is_fake_ip("198.18.0.1"));
  EXPECT_FALSE(is_fake_ip("198.20.0.1"));
  EXPECT_FALSE(is_fake_ip("111.206.174.3"));
}

// ---- server list --------------------------------------------------------------

TEST(NatDiscoveryServers, EmptySelectsTheDefaultsAndNoneDisables) {
  const auto defaults = parse_server_list("", region_e::china);
  EXPECT_FALSE(defaults.disabled);
  EXPECT_FALSE(defaults.configured);
  ASSERT_FALSE(defaults.servers.empty());
  EXPECT_EQ(defaults.servers.front().host, "stun.miwifi.com");
  EXPECT_EQ(parse_server_list("", region_e::global).servers.front().host, "stun.cloudflare.com");
  EXPECT_TRUE(parse_server_list("stun.example.net", region_e::china).configured);
  EXPECT_TRUE(parse_server_list(" none ").disabled);
  EXPECT_TRUE(parse_server_list("OFF").disabled);
}

TEST(NatDiscoveryServers, ParsesHostsPortsAndLiterals) {
  const auto list = parse_server_list("stun.example.net, 198.51.100.9:3479 [2001:db8::1]:5349");
  ASSERT_EQ(list.servers.size(), 3u);
  EXPECT_EQ(list.servers[0].host, "stun.example.net");
  EXPECT_EQ(list.servers[0].port, DEFAULT_PORT);
  EXPECT_EQ(list.servers[1].port, 3479);
  EXPECT_EQ(list.servers[2].host, "2001:db8::1");
  EXPECT_EQ(list.servers[2].port, 5349);
  EXPECT_TRUE(list.rejected.empty());
  EXPECT_EQ(join_servers(list.servers), "stun.example.net:3478, 198.51.100.9:3479, [2001:db8::1]:5349");
}

TEST(NatDiscoveryServers, RejectsUrls) {
  const auto list = parse_server_list("stun:stun.example.net, https://x.example");
  EXPECT_FALSE(list.rejected.empty());
}

// ---- overview -----------------------------------------------------------------

TEST(NatDiscoveryOverview, NothingIsReportedAsVerified) {
  discovery_t discovery;
  const auto snapshot = snapshot_with(reachability::verdict_e::verified, { "hairpin_verified" }, "203.0.113.56", reachability::ip_class_e::public_address);
  discovery.ipv4.public_address = "203.0.113.56";
  const auto result = assess_ipv4(snapshot, discovery);
  EXPECT_EQ(result.verdict, reach_e::likely);
}

TEST(NatDiscoveryOverview, AnotherAddressAboveAPublicWanMakesIpv4Unknown) {
  discovery_t discovery;
  discovery.ipv4.public_address = "198.51.100.88";
  const auto snapshot = snapshot_with(reachability::verdict_e::likely, { "mappings_complete" }, "203.0.113.56", reachability::ip_class_e::public_address);
  const auto result = assess_ipv4(snapshot, discovery);
  EXPECT_EQ(result.verdict, reach_e::unknown);
  EXPECT_EQ(result.reasons.back(), "upstream_nat");
  EXPECT_EQ(assess_upstream_nat(snapshot, discovery).state, "yes");
  EXPECT_EQ(assess_cgnat(snapshot, discovery).state, "unknown");
}

TEST(NatDiscoveryOverview, RecentSessionKeepsIpv4LikelyAboveAnUpstreamNat) {
  discovery_t discovery;
  discovery.ipv4.public_address = "198.51.100.88";
  const auto snapshot = snapshot_with(reachability::verdict_e::verified, { "session_evidence" }, "203.0.113.56", reachability::ip_class_e::public_address);
  EXPECT_EQ(assess_ipv4(snapshot, discovery).verdict, reach_e::likely);
}

TEST(NatDiscoveryOverview, SharedWanAddressIsCgnat) {
  discovery_t discovery;
  discovery.ipv4.public_address = "198.51.100.88";
  const auto snapshot = snapshot_with(reachability::verdict_e::unreachable, { "cgnat" }, "100.72.1.9", reachability::ip_class_e::cgnat);
  EXPECT_EQ(assess_cgnat(snapshot, discovery).state, "yes");
  EXPECT_EQ(assess_upstream_nat(snapshot, discovery).state, "yes");
  EXPECT_EQ(assess_ipv4(snapshot, discovery).verdict, reach_e::unreachable);
}

TEST(NatDiscoveryOverview, StunMatchingTheWanRulesOutCgnat) {
  discovery_t discovery;
  discovery.ipv4.public_address = "203.0.113.56";
  const auto snapshot = snapshot_with(reachability::verdict_e::likely, {}, "203.0.113.56", reachability::ip_class_e::public_address);
  EXPECT_EQ(assess_cgnat(snapshot, discovery).state, "no");
  EXPECT_EQ(assess_upstream_nat(snapshot, discovery).state, "no");
}

TEST(NatDiscoveryOverview, NoRouterAndNoStunLeavesCgnatUnknown) {
  discovery_t discovery;
  const auto snapshot = snapshot_with(reachability::verdict_e::unknown, { "no_igd" }, "", reachability::ip_class_e::unknown);
  EXPECT_EQ(assess_cgnat(snapshot, discovery).state, "unknown");
  EXPECT_EQ(assess_upstream_nat(snapshot, discovery).state, "unknown");
}

TEST(NatDiscoveryOverview, Ipv6WithoutAGlobalAddressIsUnreachable) {
  discovery_t discovery;
  discovery.interfaces = { ethernet({ v6("fd00::5", lifetime_e::stable) }) };
  const auto result = assess_ipv6(snapshot_with(reachability::verdict_e::unknown, {}, "", reachability::ip_class_e::unknown), discovery);
  EXPECT_EQ(result.verdict, reach_e::unreachable);
  EXPECT_EQ(result.reasons.front(), "ipv6_no_global");
}

TEST(NatDiscoveryOverview, Ipv6StableAndListeningIsStillUnknownBecauseOfTheRouterFilter) {
  discovery_t discovery;
  discovery.interfaces = { ethernet({ v6("2408:8207:1:2::abcd", lifetime_e::stable) }) };
  discovery.ipv6.public_address = "2408:8207:1:2::abcd";
  discovery.ipv6.on_interface = true;
  const auto result = assess_ipv6(snapshot_with(reachability::verdict_e::unknown, {}, "", reachability::ip_class_e::unknown), discovery);
  EXPECT_EQ(result.verdict, reach_e::unknown);
  EXPECT_EQ(result.reasons.front(), "ipv6_stable_global");
  EXPECT_EQ(result.reasons.back(), "ipv6_inbound_filter_unknown");
}

TEST(NatDiscoveryOverview, Ipv6ListenerOnIpv4OnlyIsUnreachable) {
  discovery_t discovery;
  discovery.interfaces = { ethernet({ v6("2408:8207:1:2::abcd", lifetime_e::stable) }) };
  auto snapshot = snapshot_with(reachability::verdict_e::unknown, {}, "", reachability::ip_class_e::unknown);
  for (auto &port : snapshot.ports) port.bound = { "0.0.0.0" };
  const auto result = assess_ipv6(snapshot, discovery);
  EXPECT_EQ(result.verdict, reach_e::unreachable);
  EXPECT_EQ(result.reasons.back(), "ipv6_not_listening");
}

TEST(NatDiscoveryOverview, Ipv6SessionEvidenceIsLikely) {
  discovery_t discovery;
  discovery.interfaces = { ethernet({ v6("2408:8207:1:2::abcd", lifetime_e::stable) }) };
  auto snapshot = snapshot_with(reachability::verdict_e::verified, { "session_evidence" }, "", reachability::ip_class_e::unknown);
  snapshot.public_reachability.evidence.push_back({ "client-connected-from-public", "2408:8456:1:2::/64", "2026-11-04T00:00:00Z" });
  EXPECT_EQ(assess_ipv6(snapshot, discovery).verdict, reach_e::likely);
}

TEST(NatDiscoveryOverview, Ipv6TranslatedAddressIsUnknown) {
  discovery_t discovery;
  discovery.interfaces = { ethernet({ v6("2408:8207:1:2::abcd", lifetime_e::stable) }) };
  discovery.ipv6.public_address = "2408:9999::1";
  const auto result = assess_ipv6(snapshot_with(reachability::verdict_e::unknown, {}, "", reachability::ip_class_e::unknown), discovery);
  EXPECT_EQ(result.verdict, reach_e::unknown);
  EXPECT_EQ(result.reasons.back(), "ipv6_translated");
}

TEST(NatDiscoveryOverview, ContractShape) {
  discovery_t discovery;
  discovery.checked_at = "2026-11-04T08:00:00Z";
  discovery.interfaces = { ethernet({ v6("2408:8207:1:2::abcd", lifetime_e::stable) }) };
  discovery.ipv4.socket_ok = true;
  discovery.ipv4.public_address = "203.0.113.56";
  discovery.ipv4.distinct_addresses = { "203.0.113.56" };
  discovery.ipv4.mapping.nat_present = true;
  discovery.ipv4.mapping.mapping = mapping_e::endpoint_independent;
  discovery.ipv4.mapping.method = method_e::rfc5780;
  const auto snapshot = snapshot_with(reachability::verdict_e::likely, { "mappings_complete" }, "203.0.113.56", reachability::ip_class_e::public_address);
  const auto json = overview_json(discovery, snapshot);
  EXPECT_EQ(json["version"], JSON_VERSION);
  EXPECT_EQ(json["ipv4"]["public_address"], "203.0.113.56");
  EXPECT_EQ(json["ipv4"]["source"], "stun");
  EXPECT_EQ(json["ipv4"]["nat"]["mapping"], "endpoint-independent");
  EXPECT_EQ(json["ipv4"]["nat"]["method"], "rfc5780");
  EXPECT_EQ(json["ipv4"]["nat"]["filtering"], "unknown");
  EXPECT_EQ(json["ipv6"]["public_address"], "2408:8207:1:2::abcd");
  EXPECT_EQ(json["ipv6"]["source"], "interface");
  EXPECT_EQ(json["interfaces"][0]["addresses"][0]["lifetime"], "stable");
  EXPECT_EQ(json["cgnat"]["state"], "no");
  EXPECT_EQ(json["upnp"]["available"], true);
  EXPECT_EQ(json["firewall"]["state"], "allowed");
  EXPECT_EQ(json["reachability"]["ipv4"]["verdict"], "likely");
  EXPECT_EQ(json["ipv4"]["stun_address"], "203.0.113.56");
  // NAT-PMP and PCP are not queried, so the contract has no fields for them.
  EXPECT_FALSE(json.contains("pcp"));
  EXPECT_FALSE(json.contains("nat_pmp"));
}

TEST(NatDiscoveryOverview, Ipv6ShowsTheStableAddressWhenTheServersSawATemporaryOne) {
  discovery_t discovery;
  discovery.interfaces = { ethernet({ v6("2408:8207:1:2:aaaa:bbbb:cccc:dddd", lifetime_e::temporary), v6("2408:8207:1:2::abcd", lifetime_e::stable) }) };
  discovery.ipv6.public_address = "2408:8207:1:2:aaaa:bbbb:cccc:dddd";
  discovery.ipv6.on_interface = true;
  const auto json = overview_json(discovery, snapshot_with(reachability::verdict_e::unknown, {}, "", reachability::ip_class_e::unknown));
  EXPECT_EQ(json["ipv6"]["public_address"], "2408:8207:1:2::abcd");
  EXPECT_EQ(json["ipv6"]["stun_address"], "2408:8207:1:2:aaaa:bbbb:cccc:dddd");
  EXPECT_EQ(json["ipv6"]["source"], "interface");
}

TEST(NatDiscoveryServers, RegionOrdersTheBuiltInList) {
  std::vector<std::string> china, global;
  for (const auto &s : default_servers(region_e::china)) china.push_back(s.host);
  for (const auto &s : default_servers(region_e::global)) global.push_back(s.host);
  EXPECT_EQ(china, (std::vector<std::string> { "stun.miwifi.com", "stun.chat.bilibili.com", "stun.douyucdn.cn", "stun.hitv.com", "stun.cloudflare.com" }));
  EXPECT_EQ(global, (std::vector<std::string> { "stun.cloudflare.com", "stun.l.google.com", "stun.miwifi.com", "stun.chat.bilibili.com", "stun.douyucdn.cn", "stun.hitv.com" }));
}

TEST(NatDiscoveryServers, ChinaIsAZhCnLocaleOrAChinaTimeZone) {
  EXPECT_EQ(region_of("zh", ""), region_e::china);
  EXPECT_EQ(region_of("zh_CN", "Europe/Berlin"), region_e::china);
  EXPECT_EQ(region_of("zh-Hans", ""), region_e::china);
  EXPECT_EQ(region_of("zh_TW", "Asia/Taipei"), region_e::global);
  EXPECT_EQ(region_of("en", "/usr/share/zoneinfo/Asia/Shanghai"), region_e::china);
  EXPECT_EQ(region_of("en", "Asia/Urumqi"), region_e::china);
  EXPECT_EQ(region_of("en", "China Standard Time"), region_e::china);
  EXPECT_EQ(region_of("en_US", "America/New_York"), region_e::global);
  EXPECT_EQ(region_of("", ""), region_e::global);
}

TEST(NatDiscoveryOverview, ContractListsTheServersItContacted) {
  discovery_t discovery;
  discovery.region = "china";
  discovery.contacted = { "stun.miwifi.com:3478", "stun.cloudflare.com:3478" };
  const auto json = overview_json(discovery, snapshot_with(reachability::verdict_e::unknown, {}, "", reachability::ip_class_e::unknown));
  EXPECT_EQ(json["stun"]["region"], "china");
  EXPECT_EQ(json["stun"]["contacted"].size(), 2u);
  EXPECT_EQ(json["stun"]["cache_seconds"], 600);
}
