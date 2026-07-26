/**
 * @file src/webhook/webhook_httpsclient.h
 * @brief HTTPS client for Webhook certificate verification control.
 */
#pragma once

#include <memory>
#include <string>

#include <Simple-Web-Server/client_https.hpp>

#include "webhook_client_base.h"

namespace webhook {

  class WebhookHttpsClient : public WebhookClientBase<SimpleWeb::HTTPS> {
    using base_t = WebhookClientBase<SimpleWeb::HTTPS>;

  public:
    WebhookHttpsClient(const std::string &server_port_path, bool verify_certificate);
    ~WebhookHttpsClient() noexcept override;

  protected:
    boost::asio::ssl::context context_;

    std::shared_ptr<Connection> create_connection() noexcept override;
    void connect(const std::shared_ptr<Session> &session) override;

  private:
    void handshake(const std::shared_ptr<Session> &session);
  };

}  // namespace webhook
