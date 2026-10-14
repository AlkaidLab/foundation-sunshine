/**
 * @file src/webhook/webhook_api.h
 * @brief Business handlers for standalone Webhook configuration and testing.
 */
#pragma once

#include <memory>
#include <string>

#include "src/http/server.h"

namespace webhook::api {
  using resp_https_t = std::shared_ptr<typename web::ServerBase<web::HTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename web::ServerBase<web::HTTPS>::Request>;

  /**
   * Handle already-authenticated Webhook requests.
   * The selected sunshine.conf path is supplied by the Web UI integration layer.
   */
  void
  get_config(resp_https_t response, const std::string &sunshine_config_file) noexcept;

  void
  save_config(
    resp_https_t response,
    req_https_t request,
    const std::string &sunshine_config_file
  ) noexcept;

  void
  test_delivery(resp_https_t response, req_https_t request) noexcept;
}  // namespace webhook::api
