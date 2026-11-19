/**
 * @file src/confighttp/apps_scan.h
 * @brief Route registration for host game library discovery.
 */
#pragma once

#include "src/confighttp/types.h"

namespace confighttp::apps_scan {
  void
  register_routes(confighttp::https_server_t &server);
}  // namespace confighttp::apps_scan
