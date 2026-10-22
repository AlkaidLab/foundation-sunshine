/**
 * @file src/confighttp/runtime.h
 * @brief Route registration for runtime.
 */
#pragma once

#include "src/confighttp/types.h"

namespace confighttp::runtime {
  void
  register_routes(confighttp::https_server_t &server);
}  // namespace confighttp::runtime
