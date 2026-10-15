/**
 * @file src/confighttp/apps.h
 * @brief Route registration for apps.
 */
#pragma once

#include "src/confighttp/types.h"

namespace confighttp::apps {
  void
  register_routes(confighttp::https_server_t &server);
}  // namespace confighttp::apps
