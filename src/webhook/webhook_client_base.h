/**
 * @file src/webhook/webhook_client_base.h
 * @brief Shared response-header-only transport for Webhook HTTP clients.
 */
#pragma once

#include <cstddef>
#include <istream>
#include <memory>
#include <string>

#include <Simple-Web-Server/client_http.hpp>

namespace webhook {

  /**
   * Webhook delivery is decided by the HTTP status and response headers. The
   * receiver response body is deliberately not part of the contract.
   *
   * Simple-Web-Server normally waits for the complete body before invoking
   * the request callback. Webhook clients use this isolated path so a slow or
   * streaming body cannot occupy an in-flight slot or turn an accepted POST
   * into a timeout retry.
   */
  template <class socket_type>
  class WebhookClientBase : public SimpleWeb::ClientBase<socket_type> {
    using base_t = SimpleWeb::ClientBase<socket_type>;

  protected:
    using Connection = typename base_t::Connection;
    using Session = typename base_t::Session;

    WebhookClientBase(
      const std::string &server_port_path,
      unsigned short default_port
    ) noexcept:
        base_t(server_port_path, default_port) {
    }

    void
    write_request(const std::shared_ptr<Session> &session) {
      session->connection->set_timeout(this->config.timeout);
      boost::asio::async_write(
        *session->connection->socket,
        session->request_streambuf->data(),
        [this, session](
          const SimpleWeb::error_code &write_error,
          std::size_t
        ) {
          auto lock = session->connection->handler_runner->continue_lock();
          if (!lock) {
            return;
          }
          if (write_error) {
            session->callback(write_error);
            return;
          }
          read_response_headers(session);
        }
      );
    }

  private:
    static constexpr std::size_t MAX_INTERIM_RESPONSES = 8;

    void
    read_response_headers(const std::shared_ptr<Session> &session) {
      auto response_buffer = std::make_shared<boost::asio::streambuf>(
        this->config.max_response_streambuf_size
      );
      read_response_headers(session, std::move(response_buffer), 0);
    }

    void
    read_response_headers(
      const std::shared_ptr<Session> &session,
      const std::shared_ptr<boost::asio::streambuf> &response_buffer,
      std::size_t interim_response_count
    ) {
      boost::asio::async_read_until(
        *session->connection->socket,
        *response_buffer,
        SimpleWeb::HeaderEndMatch(),
        [this, session, response_buffer, interim_response_count](
          const SimpleWeb::error_code &read_error,
          std::size_t
        ) {
          auto lock = session->connection->handler_runner->continue_lock();
          if (!lock) {
            return;
          }
          if (read_error) {
            session->callback(read_error);
            return;
          }
          std::istream response_stream(response_buffer.get());
          if (!SimpleWeb::ResponseMessage::parse(
                response_stream,
                session->response->http_version,
                session->response->status_code,
                session->response->header
              )) {
            session->callback(SimpleWeb::make_error_code::make_error_code(
              SimpleWeb::errc::protocol_error
            ));
            return;
          }
          const auto &status = session->response->status_code;
          const bool interim_response =
            status.size() >= 3 &&
            status[0] == '1' &&
            status[1] >= '0' && status[1] <= '9' &&
            status[2] >= '0' && status[2] <= '9' &&
            status.compare(0, 3, "101") != 0;
          if (interim_response) {
            if (interim_response_count >= MAX_INTERIM_RESPONSES) {
              session->callback(SimpleWeb::make_error_code::make_error_code(
                SimpleWeb::errc::protocol_error
              ));
              return;
            }
            session->response->http_version.clear();
            session->response->status_code.clear();
            session->response->header.clear();
            read_response_headers(
              session,
              response_buffer,
              interim_response_count + 1
            );
            return;
          }

          // The request callback closes this client after applying status and
          // Retry-After policy. Any already buffered body bytes are ignored.
          session->callback(SimpleWeb::error_code {});
        }
      );
    }
  };

}  // namespace webhook
