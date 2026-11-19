/**
 * @file src/network_pin.h
 * @brief Pins the outgoing sockets of the network check to the physical adapter, so a proxy that holds the default route with a tunnel adapter (TUN) cannot take them.
 *
 * The host never reads the system proxy settings: the probes use plain TCP and UDP sockets (Boost.Asio). What takes them is a TUN proxy that owns the default route, so "bypass the
 * proxy" means "leave through the physical adapter". The adapter is chosen by `choose_physical` (up, with a gateway, Ethernet or Wi-Fi, not a tunnel or virtual adapter, the lowest
 * metric) and each outgoing socket is fixed to it before it connects or sends:
 *   - Windows: IP_UNICAST_IF (IPv4, the interface index in network byte order) and IPV6_UNICAST_IF (IPv6, host byte order), see the IPPROTO_IP socket options of Winsock;
 *   - Linux: SO_BINDTODEVICE with the interface name (socket(7));
 *   - other systems: nothing is pinned and no adapter is found (`unavailable`).
 *
 * When a pinned probe gets no answer at all, `run_pinned` repeats it on the system route and reports `fallback`, so a user whose only way out is the tunnel gets the result the system route gives.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace network_pin {
  /// What an adapter is, for the choice of the one to leave through.
  enum class kind_e {
    ethernet,
    wifi,
    tunnel,  ///< a TUN/TAP/WireGuard/VPN or PPP adapter
    virtual_adapter,  ///< a container, VM or bridge adapter
    loopback,
    other,
  };

  struct adapter_t {
    std::string name;  ///< the friendly name (Windows) or the interface name (Linux)
    std::string description;  ///< Windows adapter description; empty on Linux
    std::uint32_t index = 0;  ///< the interface index (IPv4)
    std::uint32_t index_v6 = 0;  ///< the interface index for IPv6 when the system numbers it separately (Windows `Ipv6IfIndex`); 0: the same as `index`
    kind_e kind = kind_e::other;
    bool up = false;
    bool has_gateway = false;  ///< a default route goes through it
    std::uint32_t metric = 0;  ///< lower is preferred
  };

  /// The adapter to leave through: up, with a gateway, Ethernet or Wi-Fi (not a tunnel or virtual adapter), the lowest metric (the lower index on a tie). Empty when there is none.
  std::optional<adapter_t>
  choose_physical(const std::vector<adapter_t> &adapters);

  // ---- classification (pure; the platform enumerators fill the facts) -------------------------

  /// IfType values of ipifcons.h (IF_TYPE_PPP, IF_TYPE_SOFTWARE_LOOPBACK, IF_TYPE_ETHERNET_CSMACD, IF_TYPE_PROP_VIRTUAL, IF_TYPE_IEEE80211, IF_TYPE_TUNNEL); named differently because those names are macros on Windows.
  constexpr unsigned WINDOWS_IFTYPE_PPP = 23;
  constexpr unsigned WINDOWS_IFTYPE_LOOPBACK = 24;
  constexpr unsigned WINDOWS_IFTYPE_ETHERNET = 6;
  constexpr unsigned WINDOWS_IFTYPE_PROP_VIRTUAL = 53;
  constexpr unsigned WINDOWS_IFTYPE_WIFI = 71;
  constexpr unsigned WINDOWS_IFTYPE_TUNNEL = 131;

  /// A Windows adapter by its IfType and its friendly name plus description (wintun, tap, clash, wireguard, hyper-v, vmware ... mark tunnels and virtual adapters).
  kind_e
  classify_windows(unsigned if_type, std::string_view text);

  struct linux_facts_t {
    std::string name;
    bool has_device = false;  ///< /sys/class/net/<name>/device exists: backed by hardware
    bool wireless = false;  ///< /sys/class/net/<name>/wireless or phy80211 exists
    int arphrd_type = -1;  ///< /sys/class/net/<name>/type (1: Ethernet); -1 when unknown
    bool tun_tap = false;  ///< /sys/class/net/<name>/tun_flags exists: a TUN/TAP device of the kernel, whatever its name
  };

  /// A Linux interface: a name with a tunnel or container prefix and no hardware behind it is a tunnel or virtual adapter; otherwise Wi-Fi when wireless, Ethernet by its ARP type (a bridge that carries the host address counts).
  kind_e
  classify_linux(const linux_facts_t &facts);

  struct default_route_t {
    std::string iface;
    std::uint32_t metric = 0;
    bool gateway = false;
  };

  /// The default routes of /proc/net/route (IPv4) and /proc/net/ipv6_route (IPv6).
  std::vector<default_route_t>
  parse_default_routes_v4(std::string_view text);
  std::vector<default_route_t>
  parse_default_routes_v6(std::string_view text);

  /// The adapters of this computer with their routes. Windows: GetAdaptersAddresses; Linux: /proc/net/route and /sys/class/net; other systems: none.
  std::vector<adapter_t>
  enumerate_adapters();

  /// True when a probe to this host should be pinned: a host name, or a public address. Loopback, LAN, link-local, CGNAT, ULA and the 198.18.0.0/15 fake-IP range are reached through whatever route holds them, and
  /// pinning them to the physical adapter would only fail. `host` has no port; an IPv6 literal may be in brackets.
  bool
  pinnable_host(std::string_view host);

  /// `pinnable_host` for the host of a URL ("https://host:port/path").
  bool
  pinnable_url(std::string_view url);

  // ---- pinning --------------------------------------------------------------------------------

#ifdef _WIN32
  using native_handle_t = std::uintptr_t;  ///< a SOCKET
#else
  using native_handle_t = int;
#endif

  struct pin_t {
    std::uint32_t index = 0;
    std::uint32_t index_v6 = 0;  ///< for IPv6 sockets; 0: `index`
    std::string name;
    int family = 0;  ///< 4, 6 or 0 (either); `pin_socket` takes the family of the socket as its own argument
  };

  /// Pin an open, unconnected socket of `family` (4 or 6) to the adapter. False when the system refuses it or does not support it (macOS and others); never throws.
  bool
  pin_socket(native_handle_t handle, int family, const pin_t &pin);

  /// `choose_physical(enumerate_adapters())` as a pin; the answer is kept for a few seconds so that the parallel probes of a run do not enumerate again.
  std::optional<pin_t>
  find_physical();

  /// The same hook in the shape the HTTP client takes (`web::ClientConfig::on_socket_open`): pins the socket and sets `failed` when the system refused.
  std::function<void(int, std::uintptr_t)>
  socket_hook(const pin_t &pin, std::shared_ptr<bool> failed);

  // ---- outcome of a pinned probe ----------------------------------------------------------------

  enum class state_e {
    pinned,  ///< the probe left through the physical adapter (the system route was tried too when nothing answered); it does not say that anything was reached
    fallback,  ///< nothing answered through the physical adapter; the system route was used and answered
    off,  ///< the configuration turned it off
    unavailable,  ///< no physical adapter was found, the system refused to pin, or this system cannot pin
  };

  std::string_view
  to_string(state_e value);

  struct outcome_t {
    state_e state = state_e::off;
    std::string adapter_name;  ///< the physical adapter; empty when none
  };

  /// What one attempt of a probe found.
  struct attempt_t {
    bool answered = false;  ///< something answered, or there was nothing to ask (a failed attempt is `false`: only that is repeated on the system route)
    bool pin_failed = false;  ///< the pin could not be set and the attempt ran on the system route
  };

  /// The probe: runs once with the pin, or with `nullptr` on the system route.
  using attempt_fn = std::function<attempt_t(const pin_t *pin)>;

  /**
   * Runs `probe` according to the setting. `bypass` false: once on the system route (`off`). No adapter: once on the system route (`unavailable`). Otherwise pinned; when nothing answers, once more on the
   * system route: `fallback` if that answers, and `pinned` if it does not either (the network is down, not the adapter). `find` supplies the pin; tests give a fake.
   */
  outcome_t
  run_pinned(bool bypass, const std::function<std::optional<pin_t>()> &find, const attempt_fn &probe);

  /// `run_pinned` with the real adapter, and the outcome is noted for the run (see begin_run).
  outcome_t
  attempt(bool bypass, const attempt_fn &probe);

  /// The setting: "enabled" turns the bypass on; anything else is the system route.
  bool
  enabled(std::string_view setting);

  // ---- the outcome of a run of the network check -------------------------------------------------

  /// A run starts: the outcome is `off`, or `pinned`/`unavailable` from the adapter found now, until a probe notes a `fallback`.
  void
  begin_run(bool bypass);

  /// The probes of the run note what they did; `fallback` wins over `unavailable`, which wins over `pinned`.
  void
  note(const outcome_t &outcome);

  /// The outcome of the run so far.
  outcome_t
  run_outcome();

  /// Merge two outcomes by the rule of `note`.
  outcome_t
  merge(const outcome_t &current, const outcome_t &next);
}  // namespace network_pin
