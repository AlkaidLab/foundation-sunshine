/**
 * @file src/confighttp/capture_control.h
 * @brief Local control API for per-session window-follow capture (Plan4).
 */
#pragma once

#include "types.h"

namespace confighttp::capture_control {
  /**
   * @brief Register the local capture-control routes.
   *
   * Routes are only registered on Windows; on other platforms the paths stay
   * absent (404) because the window-follow backend is Windows/WGC only.
   */
  void
  register_routes(https_server_t &server);
}  // namespace confighttp::capture_control
