/**
 * @file src/network_pin.cpp
 * @brief Choice of the physical adapter and the pinning of sockets to it; see network_pin.h.
 */
#include "network_pin.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <iphlpapi.h>
  #include <windows.h>
#else
  #include <arpa/inet.h>
  #include <sys/socket.h>
  #ifdef __linux__
    #include <net/if.h>
  #endif
#endif

using namespace std::literals;

namespace network_pin {
  namespace {
    std::string
    lower(std::string_view text) {
      std::string result { text };
      std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return result;
    }

    bool
    starts_with(const std::string &text, std::string_view prefix) {
      return text.compare(0, prefix.size(), prefix) == 0;
    }

    /// The letters and digits of a text as words: "TAP-Windows Adapter V9" is tap, windows, adapter, v9.
    std::vector<std::string>
    words_of(const std::string &text) {
      std::vector<std::string> words;
      std::string word;
      for (const char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
          word.push_back(c);
        }
        else if (!word.empty()) {
          words.push_back(std::move(word));
          word.clear();
        }
      }
      if (!word.empty()) words.push_back(std::move(word));
      return words;
    }

    bool
    has_word(const std::vector<std::string> &words, std::string_view word) {
      return std::find(words.begin(), words.end(), word) != words.end();
    }

    bool
    contains(const std::string &text, std::string_view part) {
      return text.find(part) != std::string::npos;
    }

    bool
    parse_hex(const std::string &text, std::uint32_t &value) {
      if (text.empty() || text.size() > 8) return false;
      char *end = nullptr;
      const auto parsed = std::strtoul(text.c_str(), &end, 16);
      if (end == text.c_str() || *end != '\0') return false;
      value = static_cast<std::uint32_t>(parsed);
      return true;
    }

    std::mutex find_mutex;
    std::optional<pin_t> found_pin;
    std::chrono::steady_clock::time_point found_at;
    bool found_valid = false;
    constexpr auto FIND_KEEP = 3s;

    std::mutex run_mutex;
    outcome_t run_state;
    bool run_active = false;
  }  // namespace

  std::optional<adapter_t>
  choose_physical(const std::vector<adapter_t> &adapters) {
    const adapter_t *best = nullptr;
    for (const auto &adapter : adapters) {
      if (!adapter.up || !adapter.has_gateway) continue;
      if (adapter.kind != kind_e::ethernet && adapter.kind != kind_e::wifi) continue;
      if (!best || adapter.metric < best->metric || (adapter.metric == best->metric && adapter.index < best->index)) best = &adapter;
    }
    if (!best) return std::nullopt;
    return *best;
  }

  kind_e
  classify_windows(unsigned if_type, std::string_view text) {
    if (if_type == WINDOWS_IFTYPE_LOOPBACK) return kind_e::loopback;
    if (if_type == WINDOWS_IFTYPE_TUNNEL || if_type == WINDOWS_IFTYPE_PPP) return kind_e::tunnel;
    if (if_type == WINDOWS_IFTYPE_PROP_VIRTUAL) return kind_e::virtual_adapter;
    const auto name = lower(text);
    const auto words = words_of(name);
    // Words that are short enough to appear inside an unrelated name stand alone; the long ones can be matched inside a word (Wintun, "Clash for Windows").
    for (const auto word : { "tap"sv, "tun"sv, "meta"sv, "vpn"sv, "utun"sv }) {
      if (has_word(words, word)) return kind_e::tunnel;
    }
    for (const auto part : { "wintun"sv, "tunnel"sv, "clash"sv, "sing-box"sv, "v2ray"sv, "wireguard"sv, "openvpn"sv }) {
      if (contains(name, part)) return kind_e::tunnel;
    }
    for (const auto part : { "hyper-v"sv, "vethernet"sv, "vmware"sv, "virtualbox"sv, "zerotier"sv, "tailscale"sv }) {
      if (contains(name, part)) return kind_e::virtual_adapter;
    }
    if (if_type == WINDOWS_IFTYPE_ETHERNET) return kind_e::ethernet;
    if (if_type == WINDOWS_IFTYPE_WIFI) return kind_e::wifi;
    return kind_e::other;
  }

  kind_e
  classify_linux(const linux_facts_t &facts) {
    const auto name = lower(facts.name);
    if (name == "lo") return kind_e::loopback;
    if (facts.tun_tap) return kind_e::tunnel;
    if (!facts.has_device) {
      for (const auto prefix : { "tun"sv, "tap"sv, "wg"sv, "utun"sv, "zt"sv, "tailscale"sv }) {
        if (starts_with(name, prefix)) return kind_e::tunnel;
      }
      for (const auto prefix : { "docker"sv, "veth"sv, "virbr"sv, "vmnet"sv, "br-"sv }) {
        if (starts_with(name, prefix)) return kind_e::virtual_adapter;
      }
    }
    if (facts.wireless) return kind_e::wifi;
    if (facts.arphrd_type == 1) return kind_e::ethernet;
    return kind_e::other;
  }

  std::vector<default_route_t>
  parse_default_routes_v4(std::string_view text) {
    // /proc/net/route: Iface Destination Gateway Flags RefCnt Use Metric Mask ...; all hexadecimal (the Iface is a name). Flags: RTF_UP 0x1, RTF_GATEWAY 0x2.
    std::vector<default_route_t> routes;
    std::istringstream lines { std::string { text } };
    std::string line;
    std::getline(lines, line);  // the header
    while (std::getline(lines, line)) {
      std::istringstream fields { line };
      std::string iface, destination, gateway, flags, refs, use, metric, mask;
      if (!(fields >> iface >> destination >> gateway >> flags >> refs >> use >> metric >> mask)) continue;
      std::uint32_t destination_value = 1, gateway_value = 0, flag_value = 0, metric_value = 0, mask_value = 1;
      if (!parse_hex(destination, destination_value) || !parse_hex(gateway, gateway_value) || !parse_hex(flags, flag_value) || !parse_hex(mask, mask_value)) continue;
      try {
        metric_value = static_cast<std::uint32_t>(std::stoul(metric));  // decimal
      }
      catch (...) {
        continue;
      }
      if (destination_value != 0 || mask_value != 0 || (flag_value & 0x1U) == 0) continue;
      routes.push_back({ iface, metric_value, gateway_value != 0 || (flag_value & 0x2U) != 0 });
    }
    return routes;
  }

  std::vector<default_route_t>
  parse_default_routes_v6(std::string_view text) {
    // /proc/net/ipv6_route: destination(32 hex) prefix(2 hex) source source_prefix next_hop metric(8 hex) refcnt use flags iface. RTF_UP 0x1, RTF_REJECT 0x200.
    std::vector<default_route_t> routes;
    std::istringstream lines { std::string { text } };
    std::string line;
    while (std::getline(lines, line)) {
      std::istringstream fields { line };
      std::string destination, prefix, source, source_prefix, next_hop, metric, refs, use, flags, iface;
      if (!(fields >> destination >> prefix >> source >> source_prefix >> next_hop >> metric >> refs >> use >> flags >> iface)) continue;
      if (prefix != "00" || destination != std::string(32, '0')) continue;
      std::uint32_t metric_value = 0, flag_value = 0;
      if (!parse_hex(metric, metric_value) || !parse_hex(flags, flag_value)) continue;
      if ((flag_value & 0x1U) == 0 || (flag_value & 0x200U) != 0) continue;
      routes.push_back({ iface, metric_value, next_hop != std::string(32, '0') });
    }
    return routes;
  }

  std::vector<adapter_t>
  enumerate_adapters() {
    std::vector<adapter_t> result;
#ifdef _WIN32
    const auto wide_to_utf8 = [](const wchar_t *value) {
      if (!value) return std::string {};
      const int length = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
      if (length <= 1) return std::string {};
      std::string text(static_cast<std::size_t>(length), '\0');
      WideCharToMultiByte(CP_UTF8, 0, value, -1, text.data(), length, nullptr, nullptr);
      text.pop_back();
      return text;
    };
    // GetAdaptersAddresses; GAA_FLAG_INCLUDE_GATEWAYS fills FirstGatewayAddress (Windows Vista SP1 and later).
    ULONG size = 16384;
    std::vector<unsigned char> buffer(size);
    ULONG status = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3; ++attempt) {
      status = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS,
        nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data()), &size);
      if (status != ERROR_BUFFER_OVERFLOW) break;
      buffer.resize(size);
    }
    if (status != NO_ERROR) return result;
    for (auto *adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data()); adapter; adapter = adapter->Next) {
      adapter_t entry;
      entry.index = adapter->IfIndex;
      entry.index_v6 = adapter->Ipv6IfIndex;
      entry.name = adapter->FriendlyName ? wide_to_utf8(adapter->FriendlyName) : std::string(adapter->AdapterName ? adapter->AdapterName : "");
      entry.description = wide_to_utf8(adapter->Description);
      entry.kind = classify_windows(adapter->IfType, entry.name + " " + entry.description);
      entry.up = adapter->OperStatus == IfOperStatusUp;
      // The interface metric of each family that has a gateway.
      bool has_gateway_v4 = false, has_gateway_v6 = false;
      for (auto *gateway = adapter->FirstGatewayAddress; gateway; gateway = gateway->Next) {
        const auto *address = gateway->Address.lpSockaddr;
        if (!address) continue;
        if (address->sa_family == AF_INET) has_gateway_v4 = true;
        if (address->sa_family == AF_INET6) has_gateway_v6 = true;
      }
      entry.has_gateway = has_gateway_v4 || has_gateway_v6;
      if (has_gateway_v4 && has_gateway_v6) entry.metric = std::min<std::uint32_t>(adapter->Ipv4Metric, adapter->Ipv6Metric);
      else if (has_gateway_v6) entry.metric = adapter->Ipv6Metric;
      else entry.metric = adapter->Ipv4Metric;
      result.push_back(std::move(entry));
    }
#elif defined(__linux__)
    const auto read_file = [](const std::filesystem::path &path) {
      std::ifstream file { path };
      std::ostringstream text;
      text << file.rdbuf();
      return text.str();
    };
    auto routes = parse_default_routes_v4(read_file("/proc/net/route"));
    for (auto &route : parse_default_routes_v6(read_file("/proc/net/ipv6_route"))) routes.push_back(std::move(route));

    std::error_code error;
    for (const auto &item : std::filesystem::directory_iterator("/sys/class/net", error)) {
      const auto name = item.path().filename().string();
      adapter_t entry;
      entry.name = name;
      entry.index = if_nametoindex(name.c_str());
      if (entry.index == 0) continue;
      std::error_code ignored;
      linux_facts_t facts;
      facts.name = name;
      facts.has_device = std::filesystem::exists(item.path() / "device", ignored);
      facts.wireless = std::filesystem::exists(item.path() / "wireless", ignored) || std::filesystem::exists(item.path() / "phy80211", ignored);
      facts.tun_tap = std::filesystem::exists(item.path() / "tun_flags", ignored);
      {
        std::istringstream type { read_file(item.path() / "type") };
        int value = -1;
        if (type >> value) facts.arphrd_type = value;
      }
      entry.kind = classify_linux(facts);
      {
        // operstate (RFC 2863): "up", or "unknown" for a driver that does not report it.
        std::istringstream state { read_file(item.path() / "operstate") };
        std::string value;
        state >> value;
        entry.up = value == "up" || value == "unknown";
      }
      // An adapter is ranked by its default routes that have a gateway: a route without one makes it no candidate and must not give it a better place.
      bool found_route = false;
      for (const auto &route : routes) {
        if (route.iface != name || !route.gateway) continue;
        entry.has_gateway = true;
        entry.metric = found_route ? std::min(entry.metric, route.metric) : route.metric;
        found_route = true;
      }
      result.push_back(std::move(entry));
    }
#endif
    return result;
  }

  bool
  pinnable_host(std::string_view host) {
    std::string text { host };
    if (text.size() >= 2 && text.front() == '[' && text.back() == ']') text = text.substr(1, text.size() - 2);
    if (const auto zone = text.find('%'); zone != std::string::npos) text.resize(zone);
    if (text.empty()) return false;
    unsigned char bytes[16] {};
    if (inet_pton(AF_INET, text.c_str(), bytes) == 1) {
      const unsigned a = bytes[0], b = bytes[1];
      if (a == 0 || a == 10 || a == 127 || a >= 224) return false;
      if (a == 100 && (b & 0xC0) == 64) return false;  // 100.64.0.0/10
      if (a == 169 && b == 254) return false;
      if (a == 172 && (b & 0xF0) == 16) return false;
      if (a == 192 && b == 168) return false;
      if (a == 198 && (b & 0xFE) == 18) return false;  // 198.18.0.0/15: a proxy's fake-IP pool
      return true;
    }
    if (inet_pton(AF_INET6, text.c_str(), bytes) == 1) return (bytes[0] & 0xE0) == 0x20;  // 2000::/3: global unicast
    // a host name: "localhost" (and a name under it) is this computer; any other name may lead anywhere
    const auto lowered = lower(text);
    return !(lowered == "localhost" || (lowered.size() > 10 && lowered.compare(lowered.size() - 10, 10, ".localhost") == 0));
  }

  bool
  pinnable_url(std::string_view url) {
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos) return false;
    auto authority = url.substr(scheme_end + 3);
    authority = authority.substr(0, authority.find_first_of("/?#"));
    if (const auto at = authority.rfind('@'); at != std::string_view::npos) authority = authority.substr(at + 1);
    if (!authority.empty() && authority.front() == '[') {
      const auto close = authority.find(']');
      if (close == std::string_view::npos) return false;
      return pinnable_host(authority.substr(0, close + 1));
    }
    if (const auto colon = authority.find(':'); colon != std::string_view::npos) authority = authority.substr(0, colon);
    return pinnable_host(authority);
  }

  bool
  pin_socket(native_handle_t handle, int family, const pin_t &pin) {
    try {
#ifdef _WIN32
      if (pin.index == 0) return false;
      const auto socket_handle = static_cast<SOCKET>(handle);
      if (family == 6) {
        // IPV6_UNICAST_IF takes the interface index in host byte order.
        const DWORD index = pin.index_v6 != 0 ? pin.index_v6 : pin.index;
        return setsockopt(socket_handle, IPPROTO_IPV6, IPV6_UNICAST_IF, reinterpret_cast<const char *>(&index), sizeof(index)) == 0;
      }
      // IP_UNICAST_IF takes the interface index in network byte order.
      const DWORD index = htonl(pin.index);
      return setsockopt(socket_handle, IPPROTO_IP, IP_UNICAST_IF, reinterpret_cast<const char *>(&index), sizeof(index)) == 0;
#elif defined(__linux__)
      (void) family;
      if (pin.name.empty() || pin.name.size() >= IFNAMSIZ) return false;
      // SO_BINDTODEVICE takes the interface name; before Linux 5.7 it needs CAP_NET_RAW.
      return setsockopt(handle, SOL_SOCKET, SO_BINDTODEVICE, pin.name.c_str(), static_cast<socklen_t>(pin.name.size() + 1)) == 0;
#else
      (void) handle;
      (void) family;
      (void) pin;
      return false;
#endif
    }
    catch (...) {
      return false;
    }
  }

  std::optional<pin_t>
  find_physical() {
    std::lock_guard lock { find_mutex };
    const auto now = std::chrono::steady_clock::now();
    if (found_valid && now - found_at < FIND_KEEP) return found_pin;
    found_pin.reset();
    try {
      if (const auto adapter = choose_physical(enumerate_adapters())) found_pin = pin_t { adapter->index, adapter->index_v6, adapter->name, 0 };
    }
    catch (...) {
      // no enumeration: no pin
    }
    found_valid = true;
    found_at = now;
    return found_pin;
  }

  std::function<void(int, std::uintptr_t)>
  socket_hook(const pin_t &pin, std::shared_ptr<bool> failed) {
    return [pin, failed](int family, std::uintptr_t handle) {
      if (!pin_socket(static_cast<native_handle_t>(handle), family, pin) && failed) *failed = true;
    };
  }

  std::string_view
  to_string(state_e value) {
    switch (value) {
      case state_e::pinned:
        return "pinned"sv;
      case state_e::fallback:
        return "fallback"sv;
      case state_e::off:
        return "off"sv;
      case state_e::unavailable:
        return "unavailable"sv;
    }
    return "off"sv;
  }

  outcome_t
  run_pinned(bool bypass, const std::function<std::optional<pin_t>()> &find, const attempt_fn &probe) {
    if (!bypass) {
      probe(nullptr);
      return { state_e::off, {} };
    }
    std::optional<pin_t> pin;
    try {
      pin = find();
    }
    catch (...) {
      pin.reset();
    }
    if (!pin) {
      probe(nullptr);
      return { state_e::unavailable, {} };
    }
    const auto first = probe(&*pin);
    if (first.pin_failed) return { state_e::unavailable, {} };  // the probe already ran on the system route
    if (first.answered) return { state_e::pinned, pin->name };
    const auto second = probe(nullptr);
    return { second.answered ? state_e::fallback : state_e::pinned, pin->name };
  }

  outcome_t
  attempt(bool bypass, const attempt_fn &probe) {
    const auto outcome = run_pinned(bypass, find_physical, probe);
    note(outcome);
    return outcome;
  }

  bool
  enabled(std::string_view setting) {
    return setting == "enabled"sv;
  }

  outcome_t
  merge(const outcome_t &current, const outcome_t &next) {
    if (current.state == state_e::off) return current;
    const auto weight = [](state_e state) {
      switch (state) {
        case state_e::fallback:
          return 3;
        case state_e::unavailable:
          return 2;
        case state_e::pinned:
          return 1;
        case state_e::off:
          return 0;
      }
      return 0;
    };
    if (weight(next.state) > weight(current.state)) return next;
    if (next.state == current.state && current.adapter_name.empty()) return next;
    return current;
  }

  void
  begin_run(bool bypass) {
    outcome_t initial;
    if (bypass) {
      if (const auto pin = find_physical()) initial = { state_e::pinned, pin->name };
      else initial = { state_e::unavailable, {} };
    }
    std::lock_guard lock { run_mutex };
    run_state = initial;
    run_active = true;
  }

  void
  note(const outcome_t &outcome) {
    std::lock_guard lock { run_mutex };
    if (!run_active) return;
    run_state = merge(run_state, outcome);
  }

  outcome_t
  run_outcome() {
    std::lock_guard lock { run_mutex };
    return run_state;
  }
}  // namespace network_pin
