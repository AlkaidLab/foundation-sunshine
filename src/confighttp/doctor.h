/**
 * @file src/confighttp/doctor.h
 * @brief Route registration for the host health check (体检).
 */
#pragma once

#include "src/confighttp/types.h"

namespace confighttp::doctor {
  void
  register_routes(confighttp::https_server_t &server);
}  // namespace confighttp::doctor
