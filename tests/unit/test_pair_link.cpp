/**
 * @file tests/unit/test_pair_link.cpp
 * @brief The pairing link must not advertise a bind address or the Web UI port.
 */
#include <gtest/gtest.h>

#include <src/confighttp/pair_link.h>

using confighttp::pair_link::pair_url;
using confighttp::pair_link::request_t;
using confighttp::pair_link::select_target;

namespace {
  request_t
  base_request() {
    request_t request;
    request.http_port = 47989;
    request.interface_addresses = { "172.17.0.1", "10.0.1.20", "10.8.0.5" };
    request.default_route_address = "10.0.1.20";
    return request;
  }
}  // namespace

TEST(PairLink, UnspecifiedIpv4UsesDefaultRouteLan) {
  auto request = base_request();
  request.socket_address = "0.0.0.0";
  request.host_header = "0.0.0.0:47990";
  const auto target = select_target(request);
  EXPECT_EQ(target.host, "10.0.1.20");
  EXPECT_EQ(target.port, 47989);
  EXPECT_EQ(pair_url(target, "3112", "Qian-PC"), "moonlight://pair?host=10.0.1.20&port=47989&pin=3112&name=Qian-PC");
}

TEST(PairLink, UnspecifiedIpv6UsesDefaultRouteLan) {
  auto request = base_request();
  request.socket_address = "::";
  request.host_header = "[::]:47990";
  const auto target = select_target(request);
  EXPECT_EQ(target.host, "10.0.1.20");
  EXPECT_EQ(pair_url(target, "3112", "Qian-PC").find("host=::"), std::string::npos);
}

TEST(PairLink, LoopbackUsesLanNotTheWebPort) {
  auto request = base_request();
  request.socket_address = "127.0.0.1";
  request.host_header = "localhost:60000";
  request.http_port = 59999;
  const auto target = select_target(request);
  EXPECT_EQ(target.host, "10.0.1.20");
  EXPECT_EQ(target.port, 59999);
  EXPECT_EQ(pair_url(target, "3112", "Qian-PC"), "moonlight://pair?host=10.0.1.20&port=59999&pin=3112&name=Qian-PC");
}

TEST(PairLink, ChangedBasePortIsNotTheBrowserPort) {
  auto request = base_request();
  request.socket_address = "10.0.1.20";
  request.host_header = "10.0.1.20:60000";
  request.http_port = 59999;
  const auto target = select_target(request);
  EXPECT_EQ(target.host, "10.0.1.20");
  EXPECT_EQ(target.port, 59999);
}

TEST(PairLink, Ipv6BrowserAddressIsKept) {
  auto request = base_request();
  request.socket_address = "2001:db8::5";
  request.host_header = "[2001:db8::5]:47990";
  const auto target = select_target(request);
  EXPECT_EQ(target.host, "2001:db8::5");
  EXPECT_EQ(pair_url(target, "3112", "Qian-PC"), "moonlight://pair?host=2001:db8::5&port=47989&pin=3112&name=Qian-PC");
}

TEST(PairLink, Ipv6HostHeaderWhenSocketIsLoopback) {
  auto request = base_request();
  request.socket_address = "::1";
  request.host_header = "[2001:db8::9]:60000";
  request.http_port = 59999;
  const auto target = select_target(request);
  EXPECT_EQ(target.host, "2001:db8::9");
  EXPECT_EQ(target.port, 59999);
}

TEST(PairLink, MappedIpv4IsNormalized) {
  auto request = base_request();
  request.socket_address = "::ffff:10.0.8.8";
  const auto target = select_target(request);
  EXPECT_EQ(target.host, "10.0.8.8");
}

TEST(PairLink, HostHeaderUsedWhenSocketIsUnspecifiedAndNoRoute) {
  request_t request;
  request.http_port = 47989;
  request.socket_address = "0.0.0.0";
  request.host_header = "10.1.2.3:47990";
  const auto target = select_target(request);
  EXPECT_EQ(target.host, "10.1.2.3");
}

TEST(PairLink, ExternalEndpointWinsAndKeepsItsPort) {
  auto request = base_request();
  request.socket_address = "10.0.1.20";
  request.external_host = "vpn.example.com";
  request.external_port = 48010;
  request.http_port = 59999;
  const auto target = select_target(request);
  EXPECT_EQ(target.host, "vpn.example.com");
  EXPECT_EQ(target.port, 48010);
}

TEST(PairLink, ExternalWildcardIsIgnored) {
  auto request = base_request();
  request.socket_address = "0.0.0.0";
  request.external_host = "0.0.0.0";
  const auto target = select_target(request);
  EXPECT_EQ(target.host, "10.0.1.20");
}

TEST(PairLink, NoReachableAddressProducesNoLink) {
  request_t request;
  request.http_port = 47989;
  request.socket_address = "127.0.0.1";
  request.host_header = "localhost";
  request.interface_addresses = { "fe80::1", "0.0.0.0", "::" };
  EXPECT_TRUE(select_target(request).host.empty());
  EXPECT_TRUE(pair_url(select_target(request), "3112", "Qian-PC").empty());
}

TEST(PairLink, NameIsQueryEncoded) {
  auto request = base_request();
  request.socket_address = "10.0.1.20";
  const auto url = pair_url(select_target(request), "3112", "Qian PC & TV");
  EXPECT_EQ(url, "moonlight://pair?host=10.0.1.20&port=47989&pin=3112&name=Qian%20PC%20%26%20TV");
}

TEST(PairLink, LanFallbackPrefersHomePrefixWithoutDefaultRoute) {
  request_t request;
  request.http_port = 47989;
  request.socket_address = "127.0.0.1";
  request.interface_addresses = { "172.17.0.1", "10.9.0.4", "192.168.1.20", "192.0.2.8", "fd00::5" };
  const auto target = select_target(request);
  EXPECT_EQ(target.host, "192.168.1.20");
}

TEST(PairLink, TunDefaultRouteIsNotTheTarget) {
  // A proxy tool's TUN (Mihomo) takes the default route: the kernel answers 198.18.0.1 for the route, and the QR code must still carry the LAN address of the adapter.
  request_t request;
  request.http_port = 47989;
  request.socket_address = "127.0.0.1";
  request.host_header = "127.0.0.1:47990";
  request.interface_addresses = { "198.18.0.1", "192.168.3.172", "fe80::1" };
  request.default_route_address = "198.18.0.1";
  const auto target = select_target(request);
  EXPECT_EQ(target.host, "192.168.3.172");
  EXPECT_EQ(pair_url(target, "3112", "Qian-PC"), "moonlight://pair?host=192.168.3.172&port=47989&pin=3112&name=Qian-PC");
}

TEST(PairLink, BenchmarkRangeIsNeverAdvertised) {
  for (const char *tun : { "198.18.0.1", "198.18.255.254", "198.19.0.1", "198.19.255.254" }) {
    request_t request;
    request.http_port = 47989;
    request.socket_address = tun;
    request.host_header = std::string(tun) + ":47990";
    request.interface_addresses = { tun };
    request.default_route_address = tun;
    EXPECT_TRUE(select_target(request).host.empty()) << tun;
  }
  // the neighbours of the range are ordinary addresses
  for (const char *other : { "198.17.255.255", "198.20.0.1" }) {
    request_t request;
    request.http_port = 47989;
    request.socket_address = other;
    EXPECT_EQ(select_target(request).host, other);
  }
}

TEST(PairLink, LanAdapterComesBeforeAPublicDefaultRoute) {
  request_t request;
  request.http_port = 47989;
  request.socket_address = "127.0.0.1";
  request.interface_addresses = { "203.0.113.9", "192.168.1.5" };
  request.default_route_address = "203.0.113.9";
  EXPECT_EQ(select_target(request).host, "192.168.1.5");
  // without a LAN adapter the route's public address is what there is
  request.interface_addresses = { "203.0.113.9" };
  EXPECT_EQ(select_target(request).host, "203.0.113.9");
}

TEST(PairLink, LanDefaultRouteStillWins) {
  auto request = base_request();
  request.socket_address = "127.0.0.1";
  request.default_route_address = "10.8.0.5";
  EXPECT_EQ(select_target(request).host, "10.8.0.5");
}

