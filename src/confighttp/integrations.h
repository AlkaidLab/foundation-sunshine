/**
 * @file src/confighttp/integrations.h
 * @brief Route registration for integrations.
 */
#pragma once

#include "src/confighttp/types.h"

namespace confighttp::integrations {
  void
  register_routes(confighttp::https_server_t &server);
}  // namespace confighttp::integrations
