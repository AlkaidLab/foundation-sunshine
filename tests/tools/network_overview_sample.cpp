/**
 * @file tests/tools/network_overview_sample.cpp
 * @brief Print the network overview for the current host without starting a server.
 *
 * Usage: network_overview_sample [base_port] [stun_servers] [locale]
 * The second argument has the syntax of config stun_servers ("" for the built-in list, "none" skips STUN);
 * the third is config locale, which with the host time zone picks the order of the built-in list.
 */
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

#include "src/nat_discovery.h"
#include "src/network_reachability.h"

namespace {
  void
  add_port(reachability::probe_options_t &options, const char *proto, int base, int offset, std::string role) {
    const auto port = base + offset;
    if (port < 1024 || port > 65535) return;
    options.ports.push_back({ proto, static_cast<std::uint16_t>(port), std::move(role) });
  }
}  // namespace

int
main(int argc, char **argv) {
  const int base = argc > 1 ? std::atoi(argv[1]) : 47989;
  if (base < 1024 || base > 65530) {
    std::cerr << "base port out of range\n";
    return 2;
  }
  reachability::probe_options_t options;
  add_port(options, "tcp", base, -5, std::string(reachability::port_role::https));
  add_port(options, "tcp", base, 0, std::string(reachability::port_role::http));
  add_port(options, "tcp", base, 1, std::string(reachability::port_role::web));
  add_port(options, "tcp", base, 21, std::string(reachability::port_role::rtsp));
  add_port(options, "udp", base, 9, std::string(reachability::port_role::video));
  add_port(options, "udp", base, 10, std::string(reachability::port_role::control));
  add_port(options, "udp", base, 11, std::string(reachability::port_role::audio));
  add_port(options, "udp", base, 12, std::string(reachability::port_role::microphone));
  options.origin_web_ui_allowed = "lan";
  options.hairpin_port = static_cast<std::uint16_t>(base - 5);
  options.default_http_port = static_cast<std::uint16_t>(base);

  const auto region = nat_discovery::region_of(argc > 3 ? argv[3] : "", nat_discovery::host_time_zone());
  const auto list = nat_discovery::parse_server_list(argc > 2 ? argv[2] : "", region);
  for (const auto &rejected : list.rejected) std::cerr << "ignored server: " << rejected << '\n';
  nat_discovery::discover_options_t discover;
  discover.servers = list.servers;
  discover.enabled = !list.disabled;
  discover.region = list.configured ? "configured" : region == nat_discovery::region_e::china ? "china" : "global";

  const auto discovery = nat_discovery::discover(discover, true);
  const auto snapshot = reachability::probe(options, true);
  std::cout << nat_discovery::overview_json(discovery, snapshot).dump(2) << '\n';
  return 0;
}
