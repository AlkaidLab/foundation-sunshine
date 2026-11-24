/**
 * @file src/text_context/http.h
 * @brief HTTPS bridge for user-session text-context observations.
 */
#pragma once

#include <functional>
#include <memory>

#include "src/http/server.h"

namespace text_context::http {
  using https_server_t = web::Server<web::HTTPS>;
  using resp_https_t = std::shared_ptr<typename web::ServerBase<web::HTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename web::ServerBase<web::HTTPS>::Request>;
  using auth_fn = std::function<bool(resp_https_t, req_https_t)>;

  void register_routes(https_server_t &server, auth_fn auth);
}  // namespace text_context::http
