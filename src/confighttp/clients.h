/**
 * @file src/confighttp/clients.h
 * @brief Route registration for clients.
 */
#pragma once

#include "src/confighttp/types.h"

namespace confighttp::clients {
  void
  register_routes(confighttp::https_server_t &server);
}  // namespace confighttp::clients
