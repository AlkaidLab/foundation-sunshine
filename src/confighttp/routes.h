/**
 * @file src/confighttp/routes.h
 * @brief Compose the Web UI route modules without opening a listener.
 */
#pragma once

#include "types.h"

namespace confighttp {
  void
  register_routes(https_server_t &server);
}  // namespace confighttp
