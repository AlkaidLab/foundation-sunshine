/**
 * @file src/ds5_config_api.h
 * @brief Authenticated HTTP handlers for independent DualSense settings.
 */
#pragma once

#include <memory>
#include <string>

#include <Simple-Web-Server/server_https.hpp>

namespace ds5_config::api {
  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request>;

  void get_config(resp_https_t response, const std::string &sunshine_config_file) noexcept;
  void save_config(
    resp_https_t response,
    req_https_t request,
    const std::string &sunshine_config_file
  ) noexcept;
}  // namespace ds5_config::api
