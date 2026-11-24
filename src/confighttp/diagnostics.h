/**
 * @file src/confighttp/diagnostics.h
 * @brief Route registration for diagnostics.
 */
#pragma once

#include "src/confighttp/types.h"

namespace confighttp::diagnostics {
  void
  register_routes(confighttp::https_server_t &server);
}  // namespace confighttp::diagnostics
