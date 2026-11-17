/**
 * @file src/confighttp/systeminfo.h
 * @brief Route registration for the host system inventory.
 */
#pragma once

#include "src/confighttp/types.h"

namespace confighttp::systeminfo {
  void
  register_routes(confighttp::https_server_t &server);
}  // namespace confighttp::systeminfo
