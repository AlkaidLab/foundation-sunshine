/**
 * @file src/confighttp/gpu_driver_guard.h
 * @brief Route registration for the state of the graphics driver guard (safe mode, encoder check, driver notice).
 */
#pragma once

#include "src/confighttp/types.h"

namespace confighttp::gpu_driver_guard {
  void
  register_routes(confighttp::https_server_t &server);
}  // namespace confighttp::gpu_driver_guard
