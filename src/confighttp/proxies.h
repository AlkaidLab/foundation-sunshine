/**
 * @file src/confighttp/proxies.h
 * @brief Route registration for proxies.
 */
#pragma once

#include "src/confighttp/types.h"

namespace confighttp::proxies {
  void
  register_routes(confighttp::https_server_t &server);
}  // namespace confighttp::proxies
