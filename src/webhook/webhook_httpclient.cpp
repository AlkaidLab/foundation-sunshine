/**
 * @file src/webhook/webhook_httpclient.cpp
 * @brief HTTP client with Webhook-owned retry semantics.
 */
#include "webhook_httpclient.h"

namespace webhook {

  WebhookHttpClient::WebhookHttpClient(const std::string &server_port_path) noexcept:
      base_t(server_port_path, 80) {
  }

  std::shared_ptr<WebhookHttpClient::Connection>
  WebhookHttpClient::create_connection() noexcept {
    return std::make_shared<Connection>(handler_runner, *io_service);
  }

  void
  WebhookHttpClient::connect(const std::shared_ptr<Session> &session) {
    // get_connection() resets this flag for every new request. Clear it at
    // the virtual connect boundary so both fresh and pooled connections use
    // exactly the dispatcher-configured number of attempts.
    session->connection->attempt_reconnect = false;
    if (!config.proxy_server.empty()) {
      session->callback(SimpleWeb::make_error_code::make_error_code(
        SimpleWeb::errc::operation_not_supported
      ));
      return;
    }
    if (!session->connection->socket->lowest_layer().is_open()) {
      auto resolver = std::make_shared<boost::asio::ip::tcp::resolver>(*io_service);
      session->connection->set_timeout(config.timeout_connect);
      SimpleWeb::async_resolve(
        *resolver,
        *host_port,
        [this, session, resolver](
          const SimpleWeb::error_code &resolve_error,
          SimpleWeb::resolver_results results
        ) {
          session->connection->cancel_timeout();
          auto lock = session->connection->handler_runner->continue_lock();
          if (!lock) {
            return;
          }
          if (resolve_error) {
            session->callback(resolve_error);
            return;
          }

          session->connection->set_timeout(config.timeout_connect);
          boost::asio::async_connect(
            *session->connection->socket,
            results,
            [this, session, resolver](
              const SimpleWeb::error_code &connect_error,
              SimpleWeb::async_connect_endpoint
            ) {
              session->connection->cancel_timeout();
              auto lock = session->connection->handler_runner->continue_lock();
              if (!lock) {
                return;
              }
              if (connect_error) {
                session->callback(connect_error);
                return;
              }

              boost::asio::ip::tcp::no_delay option(true);
              SimpleWeb::error_code option_error;
              session->connection->socket->set_option(option, option_error);

              write_request(session);
            }
          );
        }
      );
      return;
    }

    write_request(session);
  }

}  // namespace webhook
