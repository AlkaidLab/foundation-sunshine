/**
 * @file src/confighttp/telemetry.h
 * @brief Telemetry routes of the Web UI server: the install identity, the events of the Panel and the Web UI, and what waits to be sent.
 */
#pragma once

#include "types.h"

namespace confighttp::telemetry_api {
  void
  register_routes(https_server_t &server);
}  // namespace confighttp::telemetry_api
