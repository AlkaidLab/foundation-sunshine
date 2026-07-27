/**
 * @file src/webhook/webhook_httpclient.h
 * @brief HTTP client with Webhook-owned retry semantics.
 */
#pragma once

#include <memory>
#include <string>

#include "webhook_client_base.h"

namespace webhook {

  /**
   * Simple-Web-Server retries a failed request once inside the client by
   * default. Webhook retries are owned by the dispatcher so attempts remain
   * bounded and visible to callers.
   */
  class WebhookHttpClient : public WebhookClientBase<SimpleWeb::HTTP> {
    using base_t = WebhookClientBase<SimpleWeb::HTTP>;

  public:
    explicit WebhookHttpClient(const std::string &server_port_path) noexcept;

  protected:
    std::shared_ptr<Connection> create_connection() noexcept override;
    void connect(const std::shared_ptr<Session> &session) override;
  };

}  // namespace webhook
