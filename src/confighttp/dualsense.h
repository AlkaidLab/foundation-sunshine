/**
 * @file src/confighttp/dualsense.h
 * @brief DualSense tuning profile API.
 */
#pragma once

#include "types.h"

namespace confighttp::dualsense {
  void
  register_routes(https_server_t &server);
}  // namespace confighttp::dualsense
