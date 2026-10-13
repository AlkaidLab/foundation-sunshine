/**
 * @file src/confighttp/configuration.h
 * @brief Route registration for configuration.
 */
#pragma once

#include "src/confighttp/types.h"

namespace confighttp::configuration {
  void
  register_routes(confighttp::https_server_t &server);
}  // namespace confighttp::configuration
