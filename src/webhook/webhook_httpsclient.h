/**
 * @file src/webhook/webhook_httpsclient.h
 * @brief HTTPS client for Webhook certificate verification control.
 */
#pragma once

#include <string>

#include <Simple-Web-Server/client_https.hpp>

namespace webhook {

  class WebhookHttpsClient : public SimpleWeb::Client<SimpleWeb::HTTPS> {
  public:
    WebhookHttpsClient(const std::string &server_port_path, bool verify_certificate);
  };

}  // namespace webhook
