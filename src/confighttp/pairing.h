/**
 * @file src/confighttp/pairing.h
 * @brief Pending-PIN pairing routes for the Web UI.
 */
#pragma once

#include "src/confighttp/types.h"
#include <string>

namespace confighttp::pairing {
  struct pin_request_t {
    std::string pin;
    std::string name;
    std::string id;
  };

  /// Validate a PIN request body. Returns an empty string when accepted.
  std::string
  validate_pin_request(const pin_request_t &request);

  void
  register_routes(https_server_t &server);
}  // namespace confighttp::pairing
