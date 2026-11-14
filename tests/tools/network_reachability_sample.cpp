/**
 * @file tests/tools/network_reachability_sample.cpp
 * @brief Print the reachability snapshot for the current host without starting a server.
 *
 * Usage: network_reachability_sample [base_port] [certificate_path] [origin] [process_id] [endpoints]
 */
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

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
  options.certificate_path = argc > 2 && std::string(argv[2]) != "-" ? argv[2] : "";
  options.origin_web_ui_allowed = argc > 3 ? argv[3] : "lan";
  options.hairpin_port = static_cast<std::uint16_t>(base - 5);
  options.process_id = argc > 4 ? static_cast<std::uint32_t>(std::atoi(argv[4])) : 0;
  options.external_endpoints = argc > 5 ? argv[5] : "";
  options.default_http_port = static_cast<std::uint16_t>(base);

  const auto snapshot = reachability::probe(options, true);
  std::cout << reachability::to_json(snapshot).dump(2) << '\n';
  return 0;
}
