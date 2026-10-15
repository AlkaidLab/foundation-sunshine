/**
 * @file src/confighttp/auth.h
 * @brief Web UI access policy and credential routes.
 */
#pragma once

#include "types.h"
#include <string>

namespace confighttp {
  bool
  authorize_browser_request(resp_https_t response, req_https_t request);
  bool
  authenticate(resp_https_t response, req_https_t request);
  bool
  require_localhost(resp_https_t response, req_https_t request, const std::string &action);
  void
  send_redirect(resp_https_t response, req_https_t request, const char *path);

  namespace auth {
    void
    register_routes(https_server_t &server);
  }
}  // namespace confighttp
