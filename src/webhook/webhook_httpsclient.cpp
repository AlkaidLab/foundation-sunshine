/**
 * @file src/webhook/webhook_httpsclient.cpp
 * @brief HTTPS client for Webhook certificate verification control.
 */
#include "webhook_httpsclient.h"

namespace webhook {

  WebhookHttpsClient::WebhookHttpsClient(const std::string &server_port_path, bool verify_certificate):
      SimpleWeb::Client<SimpleWeb::HTTPS>(server_port_path, verify_certificate) {
  }

}  // namespace webhook
