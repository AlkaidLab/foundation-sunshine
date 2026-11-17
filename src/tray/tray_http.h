#pragma once

#include <functional>
#include <memory>

#include "src/http/server.h"

namespace tray_http {
  using https_server_t = web::Server<web::HTTPS>;
  using resp_https_t = std::shared_ptr<typename web::ServerBase<web::HTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename web::ServerBase<web::HTTPS>::Request>;
  using auth_fn = std::function<bool(resp_https_t, req_https_t)>;

  /// How many connected tray-event subscribers can answer a launch splash (they asked with launch_splash_ack=1): the Panel of this build.
  int
  launch_splash_ack_listeners();

  void
  register_routes(https_server_t &server, auth_fn state_auth, auth_fn action_auth);
}  // namespace tray_http
