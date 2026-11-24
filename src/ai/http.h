/**
 * @file src/ai/http.h
 * @brief Route registration for ai_http.
 */
#pragma once

#include "src/confighttp/types.h"

namespace ai::http {
  void
  register_routes(confighttp::https_server_t &server);
}  // namespace ai::http
