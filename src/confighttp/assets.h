/**
 * @file src/confighttp/assets.h
 * @brief Route registration for assets.
 */
#pragma once

#include "src/confighttp/types.h"

namespace confighttp::assets {
  void
  register_routes(confighttp::https_server_t &server);
}  // namespace confighttp::assets
