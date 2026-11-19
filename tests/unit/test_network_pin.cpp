/**
 * @file tests/unit/test_network_pin.cpp
 * @brief The choice of the physical adapter, the classification of adapters, and the outcome of a pinned probe.
 */
#include "src/network_pin.h"

#include <gtest/gtest.h>

#ifndef _WIN32
  #include <sys/socket.h>
  #include <unistd.h>
#endif

namespace {
  using namespace network_pin;

  adapter_t
  make(std::string name, std::uint32_t index, kind_e kind, std::uint32_t metric, bool up = true, bool gateway = true, std::string description = {}) {
    adapter_t adapter;
    adapter.name = std::move(name);
    adapter.description = std::move(description);
    adapter.index = index;
    adapter.kind = kind;
    adapter.up = up;
    adapter.has_gateway = gateway;
    adapter.metric = metric;
    return adapter;
  }

  pin_t
  pin_of(std::string name = "Ethernet", std::uint32_t index = 7) {
    return { index, 0, std::move(name), 0 };
  }
}  // namespace

// ---- choose_physical -------------------------------------------------------------------------

TEST(NetworkPinChoose, TunnelHoldingTheDefaultRouteLosesToThePhysicalAdapter) {
  const auto chosen = choose_physical({
    make("Meta", 20, kind_e::tunnel, 0),  // the proxy's TUN: the best metric, the default route
    make("Ethernet", 7, kind_e::ethernet, 25),
  });
  ASSERT_TRUE(chosen);
  EXPECT_EQ(chosen->name, "Ethernet");
}

TEST(NetworkPinChoose, OnlyTunnelsAndVirtualAdaptersGivesNothing) {
  EXPECT_FALSE(choose_physical({
    make("Meta", 20, kind_e::tunnel, 0),
    make("vEthernet (WSL)", 30, kind_e::virtual_adapter, 5),
    make("Loopback", 1, kind_e::loopback, 1),
  }));
  EXPECT_FALSE(choose_physical({}));
}

TEST(NetworkPinChoose, SeveralPhysicalAdaptersTakeTheLowestMetric) {
  const auto chosen = choose_physical({
    make("Wi-Fi", 9, kind_e::wifi, 40),
    make("Ethernet", 7, kind_e::ethernet, 25),
    make("Ethernet 2", 8, kind_e::ethernet, 30),
  });
  ASSERT_TRUE(chosen);
  EXPECT_EQ(chosen->name, "Ethernet");
}

TEST(NetworkPinChoose, ATieTakesTheLowerIndex) {
  const auto chosen = choose_physical({ make("b", 9, kind_e::wifi, 10), make("a", 4, kind_e::ethernet, 10) });
  ASSERT_TRUE(chosen);
  EXPECT_EQ(chosen->index, 4U);
}

TEST(NetworkPinChoose, DownAdaptersAndAdaptersWithoutGatewayAreLeftOut) {
  const auto chosen = choose_physical({
    make("Ethernet", 7, kind_e::ethernet, 1, false),
    make("Ethernet 2", 8, kind_e::ethernet, 2, true, false),
    make("Wi-Fi", 9, kind_e::wifi, 50),
  });
  ASSERT_TRUE(chosen);
  EXPECT_EQ(chosen->name, "Wi-Fi");
  EXPECT_FALSE(choose_physical({ make("Ethernet", 7, kind_e::ethernet, 1, false) }));
}

TEST(NetworkPinChoose, AnAdapterWhoseDescriptionNamesAVirtualAdapterIsExcludedByTheClassification) {
  // the same path as the Windows enumeration: IfType Ethernet, but the description says it is not hardware
  const auto virtual_kind = classify_windows(WINDOWS_IFTYPE_ETHERNET, "vEthernet (Default Switch) Hyper-V Virtual Ethernet Adapter");
  const auto chosen = choose_physical({
    make("vEthernet (Default Switch)", 30, virtual_kind, 1),
    make("Ethernet", 7, classify_windows(WINDOWS_IFTYPE_ETHERNET, "Ethernet Intel(R) Ethernet Connection I219-V"), 25),
  });
  ASSERT_TRUE(chosen);
  EXPECT_EQ(chosen->name, "Ethernet");
}

// ---- classification --------------------------------------------------------------------------

TEST(NetworkPinClassify, WindowsByIfType) {
  EXPECT_EQ(classify_windows(WINDOWS_IFTYPE_ETHERNET, "Ethernet Realtek PCIe GbE Family Controller"), kind_e::ethernet);
  EXPECT_EQ(classify_windows(WINDOWS_IFTYPE_WIFI, "Wi-Fi Intel(R) Wi-Fi 6 AX201 160MHz"), kind_e::wifi);
  EXPECT_EQ(classify_windows(WINDOWS_IFTYPE_TUNNEL, "Teredo Tunneling Pseudo-Interface"), kind_e::tunnel);
  EXPECT_EQ(classify_windows(WINDOWS_IFTYPE_LOOPBACK, "Loopback Pseudo-Interface 1"), kind_e::loopback);
  EXPECT_EQ(classify_windows(WINDOWS_IFTYPE_PPP, "WAN Miniport (PPPOE)"), kind_e::tunnel);
  EXPECT_EQ(classify_windows(WINDOWS_IFTYPE_PROP_VIRTUAL, "Microsoft Wi-Fi Direct Virtual Adapter"), kind_e::virtual_adapter);
  EXPECT_EQ(classify_windows(1, "something else"), kind_e::other);
}

TEST(NetworkPinClassify, WindowsDescriptionKeywords) {
  for (const auto *text : { "Meta Tunnel", "Clash Wintun Userspace Tunnel", "TAP-Windows Adapter V9", "sing-box Tun", "v2rayN TUN", "WireGuard Tunnel", "OpenVPN Data Channel Offload", "Some VPN Client Adapter" }) {
    EXPECT_EQ(classify_windows(WINDOWS_IFTYPE_ETHERNET, text), kind_e::tunnel) << text;
  }
  for (const auto *text : { "Hyper-V Virtual Ethernet Adapter", "vEthernet (WSL)", "VMware Virtual Ethernet Adapter for VMnet8", "VirtualBox Host-Only Ethernet Adapter", "ZeroTier Virtual Port", "Tailscale Tunnel" }) {
    const auto kind = classify_windows(WINDOWS_IFTYPE_ETHERNET, text);
    EXPECT_TRUE(kind == kind_e::virtual_adapter || kind == kind_e::tunnel) << text;
  }
  // short keywords stand alone: ordinary hardware names that merely contain their letters are physical
  for (const auto *text : { "Ethernet Intel(R) Ethernet Connection (7) I219-V", "Realtek USB GbE Family Controller #2", "Qualcomm Atheros QCA9377 Wireless Network Adapter", "Metadata Ethernet 10G" }) {
    EXPECT_EQ(classify_windows(WINDOWS_IFTYPE_ETHERNET, text), kind_e::ethernet) << text;
  }
}

TEST(NetworkPinClassify, LinuxPrefixWithoutHardwareIsTunnelOrVirtual) {
  for (const auto *name : { "tun0", "tap1", "wg0", "utun3", "zt5u4abcd", "tailscale0" }) {
    EXPECT_EQ(classify_linux({ name, false, false, 65534 }), kind_e::tunnel) << name;
  }
  for (const auto *name : { "docker0", "veth12ab", "virbr0", "vmnet8", "br-1a2b3c" }) {
    EXPECT_EQ(classify_linux({ name, false, false, 1 }), kind_e::virtual_adapter) << name;
  }
}

TEST(NetworkPinClassify, LinuxHardwareAndBridges) {
  EXPECT_EQ(classify_linux({ "enp6s0", true, false, 1 }), kind_e::ethernet);
  EXPECT_EQ(classify_linux({ "wlp3s0", true, true, 1 }), kind_e::wifi);
  EXPECT_EQ(classify_linux({ "br0", false, false, 1 }), kind_e::ethernet);  // a bridge that carries the host's address
  EXPECT_EQ(classify_linux({ "lo", false, false, 772 }), kind_e::loopback);
  EXPECT_EQ(classify_linux({ "ppp0", false, false, 512 }), kind_e::other);
  // a hardware device whose name happens to start with a tunnel prefix is hardware
  EXPECT_EQ(classify_linux({ "wg-lan", true, false, 1 }), kind_e::ethernet);
}

TEST(NetworkPinRoutes, Ipv4DefaultRoutes) {
  const std::string text =
    "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n"
    "tun0\t00000000\t0100FEFF\t0003\t0\t0\t0\t00000000\t0\t0\t0\n"
    "enp6s0\t00000000\t0101A8C0\t0003\t0\t0\t100\t00000000\t0\t0\t0\n"
    "enp6s0\t0001A8C0\t00000000\t0001\t0\t0\t100\t00FFFFFF\t0\t0\t0\n"
    "wlp3s0\t00000000\t0101A8C0\t0001\t0\t0\t600\t00000000\t0\t0\t0\n"
    "docker0\t00000000\t00000000\t0000\t0\t0\t0\t00000000\t0\t0\t0\n";
  const auto routes = parse_default_routes_v4(text);
  ASSERT_EQ(routes.size(), 3U);
  EXPECT_EQ(routes[0].iface, "tun0");
  EXPECT_EQ(routes[0].metric, 0U);
  EXPECT_TRUE(routes[0].gateway);
  EXPECT_EQ(routes[1].iface, "enp6s0");
  EXPECT_EQ(routes[1].metric, 100U);
  EXPECT_EQ(routes[2].iface, "wlp3s0");
  EXPECT_TRUE(routes[2].gateway);  // a gateway address, although RTF_GATEWAY is not set
  EXPECT_TRUE(parse_default_routes_v4("").empty());
  EXPECT_TRUE(parse_default_routes_v4("Iface\nbroken line\n").empty());
}

TEST(NetworkPinRoutes, Ipv6DefaultRoutes) {
  const std::string text =
    "00000000000000000000000000000000 00 00000000000000000000000000000000 00 fe800000000000000000000000000001 00000400 00000001 00000000 00000003 enp6s0\n"
    "00000000000000000000000000000000 00 00000000000000000000000000000000 00 00000000000000000000000000000000 ffffffff 00000001 00000000 00200200 lo\n"
    "20010db8000000000000000000000000 20 00000000000000000000000000000000 00 00000000000000000000000000000000 00000100 00000001 00000000 00000001 enp6s0\n";
  const auto routes = parse_default_routes_v6(text);
  ASSERT_EQ(routes.size(), 1U);  // the reject route and the on-link prefix are not default routes
  EXPECT_EQ(routes[0].iface, "enp6s0");
  EXPECT_EQ(routes[0].metric, 0x400U);
  EXPECT_TRUE(routes[0].gateway);
}

// ---- run_pinned ------------------------------------------------------------------------------

TEST(NetworkPinRun, OffRunsOnceOnTheSystemRoute) {
  int calls = 0;
  bool pinned_call = false;
  bool found = false;
  const auto outcome = run_pinned(
    false, [&]() { found = true; return pin_of(); },
    [&](const pin_t *pin) { ++calls; pinned_call = pin != nullptr; return attempt_t { true, false }; });
  EXPECT_EQ(outcome.state, state_e::off);
  EXPECT_EQ(calls, 1);
  EXPECT_FALSE(pinned_call);
  EXPECT_FALSE(found);
}

TEST(NetworkPinRun, NoPhysicalAdapterIsUnavailableAndRunsOnTheSystemRoute) {
  int calls = 0;
  const auto outcome = run_pinned(
    true, []() { return std::optional<pin_t> {}; }, [&](const pin_t *pin) { ++calls; EXPECT_EQ(pin, nullptr); return attempt_t { false, false }; });
  EXPECT_EQ(outcome.state, state_e::unavailable);
  EXPECT_TRUE(outcome.adapter_name.empty());
  EXPECT_EQ(calls, 1);
}

TEST(NetworkPinRun, AnAnsweredPinnedProbeIsPinned) {
  int calls = 0;
  const auto outcome = run_pinned(
    true, []() { return pin_of("Ethernet"); }, [&](const pin_t *pin) { ++calls; EXPECT_NE(pin, nullptr); return attempt_t { true, false }; });
  EXPECT_EQ(outcome.state, state_e::pinned);
  EXPECT_EQ(outcome.adapter_name, "Ethernet");
  EXPECT_EQ(calls, 1);
}

TEST(NetworkPinRun, NoAnswerPinnedThenAnswerOnTheSystemRouteIsFallback) {
  std::vector<bool> pinned_calls;
  const auto outcome = run_pinned(
    true, []() { return pin_of("Ethernet"); }, [&](const pin_t *pin) {
      pinned_calls.push_back(pin != nullptr);
      return attempt_t { pin == nullptr, false };
    });
  EXPECT_EQ(outcome.state, state_e::fallback);
  EXPECT_EQ(outcome.adapter_name, "Ethernet");
  EXPECT_EQ(pinned_calls, (std::vector<bool> { true, false }));
}

TEST(NetworkPinRun, NoAnswerEitherWayIsStillPinned) {
  // the network is down; the adapter is not the cause, so there is no "fallback" warning
  int calls = 0;
  const auto outcome = run_pinned(
    true, []() { return pin_of(); }, [&](const pin_t *) { ++calls; return attempt_t { false, false }; });
  EXPECT_EQ(outcome.state, state_e::pinned);
  EXPECT_EQ(calls, 2);
}

TEST(NetworkPinRun, AnAttemptThatCouldNotPinIsUnavailableAndIsNotRepeated) {
  int calls = 0;
  const auto outcome = run_pinned(
    true, []() { return pin_of(); }, [&](const pin_t *) { ++calls; return attempt_t { false, true }; });
  EXPECT_EQ(outcome.state, state_e::unavailable);
  EXPECT_EQ(calls, 1);
}

TEST(NetworkPinRun, AFindThatThrowsIsUnavailable) {
  const auto outcome = run_pinned(
    true, []() -> std::optional<pin_t> { throw std::runtime_error("no enumeration"); }, [](const pin_t *) { return attempt_t { true, false }; });
  EXPECT_EQ(outcome.state, state_e::unavailable);
}

TEST(NetworkPinState, Names) {
  EXPECT_EQ(to_string(state_e::pinned), "pinned");
  EXPECT_EQ(to_string(state_e::fallback), "fallback");
  EXPECT_EQ(to_string(state_e::off), "off");
  EXPECT_EQ(to_string(state_e::unavailable), "unavailable");
  EXPECT_TRUE(enabled("enabled"));
  EXPECT_FALSE(enabled("disabled"));
  EXPECT_FALSE(enabled(""));
}

// ---- the outcome of a run ---------------------------------------------------------------------

TEST(NetworkPinMerge, FallbackWinsOverUnavailableWinsOverPinned) {
  const outcome_t pinned { state_e::pinned, "Ethernet" }, fallback { state_e::fallback, "Ethernet" }, unavailable { state_e::unavailable, {} };
  EXPECT_EQ(merge(pinned, fallback).state, state_e::fallback);
  EXPECT_EQ(merge(fallback, pinned).state, state_e::fallback);
  EXPECT_EQ(merge(pinned, unavailable).state, state_e::unavailable);
  EXPECT_EQ(merge(unavailable, fallback).state, state_e::fallback);
  EXPECT_EQ(merge(fallback, unavailable).state, state_e::fallback);
  EXPECT_EQ(merge(pinned, pinned).adapter_name, "Ethernet");
}

TEST(NetworkPinMerge, OffStaysOff) {
  const outcome_t off { state_e::off, {} };
  EXPECT_EQ(merge(off, { state_e::fallback, "Ethernet" }).state, state_e::off);
}

TEST(NetworkPinRunOutcome, BypassOffIgnoresWhatProbesNote) {
  begin_run(false);
  EXPECT_EQ(run_outcome().state, state_e::off);
  note({ state_e::fallback, "Ethernet" });
  EXPECT_EQ(run_outcome().state, state_e::off);
}

TEST(NetworkPinRunOutcome, ABypassRunStartsFromTheAdapterFoundAndTakesTheFallback) {
  begin_run(true);
  const auto start = run_outcome();
  EXPECT_TRUE(start.state == state_e::pinned || start.state == state_e::unavailable);
  note({ state_e::fallback, "Ethernet" });
  EXPECT_EQ(run_outcome().state, state_e::fallback);
  EXPECT_EQ(run_outcome().adapter_name, "Ethernet");
  begin_run(true);  // the next run starts clean
  EXPECT_NE(run_outcome().state, state_e::fallback);
  begin_run(false);
}

// ---- pin_socket ------------------------------------------------------------------------------

#ifndef _WIN32
TEST(NetworkPinSocket, AnInterfaceThatDoesNotExistFailsWithoutThrowing) {
  const int handle = ::socket(AF_INET, SOCK_DGRAM, 0);
  ASSERT_GE(handle, 0);
  bool result = true;
  EXPECT_NO_THROW(result = pin_socket(handle, 4, { 9999, 0, "nopin0", 0 }));
  EXPECT_FALSE(result);
  EXPECT_NO_THROW(result = pin_socket(handle, 6, { 9999, 0, std::string(64, 'x'), 0 }));  // longer than IFNAMSIZ
  EXPECT_FALSE(result);
  EXPECT_FALSE(pin_socket(handle, 4, { 0, 0, "", 0 }));
  ::close(handle);
}

TEST(NetworkPinSocket, AnInvalidHandleFailsWithoutThrowing) {
  EXPECT_NO_THROW(EXPECT_FALSE(pin_socket(-1, 4, { 1, 0, "eth0", 0 })));
}

TEST(NetworkPinSocket, TheHookSetsFailedWhenThePinIsRefused) {
  const int handle = ::socket(AF_INET, SOCK_DGRAM, 0);
  ASSERT_GE(handle, 0);
  auto failed = std::make_shared<bool>(false);
  const auto hook = socket_hook({ 9999, 0, "nopin0", 0 }, failed);
  hook(4, static_cast<std::uintptr_t>(handle));
  EXPECT_TRUE(*failed);
  ::close(handle);
}
#endif

TEST(NetworkPinFind, TheEnumerationDoesNotThrowAndTheAnswerIsStable) {
  std::vector<adapter_t> adapters;
  EXPECT_NO_THROW(adapters = enumerate_adapters());
  const auto first = find_physical();
  const auto second = find_physical();
  EXPECT_EQ(first.has_value(), second.has_value());
  if (first && second) {
    EXPECT_EQ(first->index, second->index);
  }
}

// ---- pinnable destinations -------------------------------------------------------------------

TEST(NetworkPinDestination, OnlyHostNamesAndPublicAddressesArePinned) {
  for (const auto *host : { "example.com", "8.8.8.8", "1.1.1.1", "223.5.5.5", "2606:4700:4700::1111", "[2001:4860:4860::8888]", "172.32.0.1", "100.128.0.1" }) {
    EXPECT_TRUE(pinnable_host(host)) << host;
  }
  for (const auto *host : { "127.0.0.1", "10.1.2.3", "192.168.1.5", "172.16.0.1", "172.31.255.1", "100.64.0.1", "100.127.1.1", "169.254.1.1", "198.18.0.5", "198.19.255.1", "0.0.0.0", "224.0.0.1", "::1", "fe80::1", "fd00::1", "[::1]", "" }) {
    EXPECT_FALSE(pinnable_host(host)) << host;
  }
}

TEST(NetworkPinDestination, TheHostOfAUrl) {
  EXPECT_TRUE(pinnable_url("https://api.example.com/v1/reach-probe"));
  EXPECT_TRUE(pinnable_url("https://api.example.com:8443/x"));
  EXPECT_TRUE(pinnable_url("https://user:pw@203.0.113.5:8443/x"));
  EXPECT_TRUE(pinnable_url("http://[2606:4700::1111]:80/x"));
  EXPECT_FALSE(pinnable_url("http://127.0.0.1:8787/v1/reach-probe"));
  EXPECT_FALSE(pinnable_url("http://192.168.1.20/x"));
  EXPECT_FALSE(pinnable_url("http://[::1]:8787/x"));
  EXPECT_FALSE(pinnable_url("not a url"));
}

TEST(NetworkPinClassify, ATunTapDeviceIsATunnelWhateverItIsCalled) {
  linux_facts_t corp;
  corp.name = "corp0";
  corp.has_device = false;
  corp.arphrd_type = 1;  // a TAP presents itself as Ethernet
  corp.tun_tap = true;
  EXPECT_EQ(classify_linux(corp), kind_e::tunnel);
  corp.tun_tap = false;
  EXPECT_EQ(classify_linux(corp), kind_e::ethernet) << "a plain bridge that carries the host address stays a candidate";
}

TEST(NetworkPinTargets, LocalhostIsThisComputerAndNeverPinned) {
  EXPECT_FALSE(pinnable_host("localhost"));
  EXPECT_FALSE(pinnable_host("LOCALHOST"));
  EXPECT_FALSE(pinnable_host("service.localhost"));
  EXPECT_TRUE(pinnable_host("reach.example.net"));
  EXPECT_FALSE(pinnable_url("http://localhost:8787/"));
  EXPECT_TRUE(pinnable_url("https://reach.example.net/"));
}

TEST(NetworkPinChoice, TheIpv6IndexTravelsWithThePin) {
  adapter_t adapter;
  adapter.name = "Ethernet";
  adapter.index = 12;
  adapter.index_v6 = 7;
  adapter.kind = kind_e::ethernet;
  adapter.up = true;
  adapter.has_gateway = true;
  const auto chosen = choose_physical({ adapter });
  ASSERT_TRUE(chosen.has_value());
  EXPECT_EQ(chosen->index, 12U);
  EXPECT_EQ(chosen->index_v6, 7U);
}
