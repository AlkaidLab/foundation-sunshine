#pragma once

#include <functional>
#include <memory>

#include <Simple-Web-Server/server_https.hpp>

namespace tray_http {
  using https_server_t = SimpleWeb::Server<SimpleWeb::HTTPS>;
  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request>;
  using auth_fn = std::function<bool(resp_https_t, req_https_t)>;

  void
  register_routes(https_server_t &server, auth_fn state_auth, auth_fn action_auth);
}  // namespace tray_http
