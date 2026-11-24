/**
 * @file src/file_mapping/config_http.h
 * @brief Route registration for file_mapping.
 */
#pragma once

#include "src/confighttp/types.h"

namespace file_mapping::config_http {
  void
  register_routes(confighttp::https_server_t &server);
}  // namespace file_mapping::config_http
