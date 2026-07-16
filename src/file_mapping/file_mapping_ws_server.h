/**
 * @file src/file_mapping_ws_server.h
 * @brief Boost.Asio listener for the file mapping WebSocket transport.
 */
#pragma once

#include <atomic>
#include <memory>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>

#include "file_mapping_ws.h"

namespace file_mapping_ws {
  namespace asio = boost::asio;
  namespace ssl = boost::asio::ssl;
  using tcp = boost::asio::ip::tcp;

  class server_t: public std::enable_shared_from_this<server_t> {
  public:
    server_t(
      asio::io_context &io,
      transport_config_t config,
      session_token_validator_t validate_token = {},
      client_uuid_authorizer_t authorize_peer_uuid = {},
      file_mapping::operations::execution_context_t operations_context = {});

    validation_result_t start();
    void stop();
    transport_state_e state() const;
    std::uint16_t bound_port() const;

  private:
    validation_result_t configure_ssl();
    validation_result_t open_acceptor();
    void accept_next();
    void on_accept(boost::system::error_code ec, tcp::socket socket);

    asio::io_context &io_;
    transport_config_t config_;
    session_token_validator_t validate_token_;
    client_uuid_authorizer_t authorize_peer_uuid_;
    file_mapping::operations::execution_context_t operations_context_;
    ssl::context ssl_ctx_;
    tcp::acceptor acceptor_;
    std::atomic<std::size_t> active_sessions_ { 0 };
    std::atomic<transport_state_e> state_ { transport_state_e::stopped };
  };
}  // namespace file_mapping_ws
