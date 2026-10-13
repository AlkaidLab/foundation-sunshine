/**
 * @file src/confighttp/types.h
 * @brief Private transport types for Web UI route modules.
 */
#pragma once

#include <Simple-Web-Server/server_https.hpp>
#include <memory>

namespace confighttp {
  using https_server_t = SimpleWeb::Server<SimpleWeb::HTTPS>;
  using resp_https_t = std::shared_ptr<https_server_t::Response>;
  using req_https_t = std::shared_ptr<https_server_t::Request>;
}  // namespace confighttp
