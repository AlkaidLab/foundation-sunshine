#pragma once

#include <memory>

#include <Simple-Web-Server/server_https.hpp>

namespace remote_connect::api {
  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request>;

  // These handlers are called after the Web UI layer has authenticated the request.
  void
  get_status(resp_https_t response);

  void
  set_enabled(resp_https_t response, req_https_t request);

  void
  reset_enrollment(resp_https_t response);

}  // namespace remote_connect::api
