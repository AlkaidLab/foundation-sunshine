/**
 * @file src/file_mapping_ws_session.h
 * @brief Boost.Beast WebSocket session wrapper for file mapping.
 */
#pragma once

#include <deque>
#include <functional>
#include <memory>
#include <string>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include "file_mapping_ws.h"

namespace file_mapping_ws {
  namespace asio = boost::asio;
  namespace beast = boost::beast;
  namespace http = boost::beast::http;
  namespace ssl = boost::asio::ssl;
  namespace websocket = boost::beast::websocket;
  using tcp = boost::asio::ip::tcp;
  using websocket_stream_t = websocket::stream<beast::ssl_stream<tcp::socket>>;

  class beast_session_t: public std::enable_shared_from_this<beast_session_t> {
  public:
    beast_session_t(
      websocket_stream_t stream,
      session_token_validator_t validate_token = {},
      client_uuid_authorizer_t authorize_peer_uuid = {},
      session_core_t core = session_core_t {},
      std::string expected_path = "/api/v1/file-mapping/session",
      file_mapping::operations::execution_context_t operations_context = {},
      transport_config_t config = {},
      std::function<void()> on_close = {});

    ~beast_session_t();

    void start();
    void close();

  private:
    void on_tls_handshake(beast::error_code ec);
    void on_ws_request(beast::error_code ec, std::size_t bytes_transferred);
    void on_ws_accept(beast::error_code ec);
    void read_next();
    void on_read(beast::error_code ec, std::size_t bytes_transferred);
    void queue_reply(outbound_frame_t frame);
    void write_next();
    void on_write(beast::error_code ec, std::size_t bytes_transferred);
    void close_with_error(const std::string &reason = {});

    websocket_stream_t ws_;
    beast::flat_buffer read_buffer_;
    http::request<http::string_body> upgrade_request_;
    session_token_validator_t validate_token_;
    client_uuid_authorizer_t authorize_peer_uuid_;
    session_core_t core_;
    std::string expected_path_;
    file_mapping::operations::execution_context_t operations_context_;
    transport_config_t config_;
    std::function<void()> on_close_;
    std::deque<outbound_frame_t> write_queue_;
    std::string pending_close_reason_;
    bool write_active_ = false;
    bool close_requested_ = false;
    bool socket_closed_ = false;
  };
}  // namespace file_mapping_ws
