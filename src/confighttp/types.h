/**
 * @file src/confighttp/types.h
 * @brief Private transport types for Web UI route modules.
 */
#pragma once

#include "src/http/server.h"
#include <memory>

namespace confighttp {
  using https_server_t = web::Server<web::HTTPS>;
  using resp_https_t = std::shared_ptr<https_server_t::Response>;
  using req_https_t = std::shared_ptr<https_server_t::Request>;
}  // namespace confighttp
