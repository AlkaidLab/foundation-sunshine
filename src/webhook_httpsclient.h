/**
 * @file src/webhook_httpsclient.h
 * @brief HTTPS client for webhook with certificate verification
 */
#pragma once

#include <string>
#include <Simple-Web-Server/client_https.hpp>

namespace webhook {

  class WebhookHttpsClient : public SimpleWeb::Client<SimpleWeb::HTTPS> {
  public:
    WebhookHttpsClient(const std::string& server_port_path, 
                      bool verify_certificate = true, 
                      const std::string& expected_hostname = "");

  private:
    std::string expected_hostname_;
  };

}  // namespace webhook
