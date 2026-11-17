/**
 * @file src/confighttp/network.h
 * @brief Route registration for network reachability.
 */
#pragma once

#include "src/confighttp/types.h"
#include "src/network_reachability.h"

namespace confighttp::network {
  /** The reachability probe for this host's configured ports, certificate and addresses. */
  reachability::probe_options_t
  configured_probe();

  void
  register_routes(confighttp::https_server_t &server);
}  // namespace confighttp::network
