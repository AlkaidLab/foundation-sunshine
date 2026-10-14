#pragma once

#include <functional>
#include <memory>

#include "src/http/server.h"

namespace widget_http {
  using https_server_t = web::Server<web::HTTPS>;
  using resp_https_t = std::shared_ptr<typename web::ServerBase<web::HTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename web::ServerBase<web::HTTPS>::Request>;
  using auth_fn = std::function<bool(resp_https_t, req_https_t)>;

  /**
   * @brief Registers the local Game Bar widget endpoints on the config UI server.
   *
   * Routes are only active when `widget_token` is set in sunshine.conf. Access is
   * additionally restricted by the injected auth_fn (loopback gate); the per-request
   * credential is the X-Sunshine-Token header matching widget_token.
   */
  void
  register_routes(https_server_t &server, auth_fn auth);
}  // namespace widget_http
