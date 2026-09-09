/**
 * @file src/hdr_enhanced/api.h
 * @brief HDR management handlers behind confighttp authentication.
 */
#pragma once
#include <Simple-Web-Server/server_https.hpp>
#include <memory>

namespace hdr_enhanced::api {
  using response_t = std::shared_ptr<SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Response>;
  using request_t = std::shared_ptr<SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request>;

  void
  get_config(response_t response) noexcept;
  void
  save_config(response_t response, request_t request) noexcept;
  void
  get_status(response_t response) noexcept;
  void
  maintenance(response_t response, request_t request) noexcept;
  void
  shutdown() noexcept;
}  // namespace hdr_enhanced::api
