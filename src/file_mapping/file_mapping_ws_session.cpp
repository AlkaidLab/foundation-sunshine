/**
 * @file src/file_mapping_ws_session.cpp
 * @brief Boost.Beast WebSocket session wrapper for file mapping.
 */
#include "file_mapping_ws_session.h"

#include <algorithm>
#include <utility>
#include <vector>

#include <boost/asio/buffer.hpp>
#include <boost/beast/core/buffers_to_string.hpp>

namespace file_mapping_ws {
  beast_session_t::beast_session_t(
    websocket_stream_t stream,
    session_token_validator_t validate_token,
    client_uuid_authorizer_t authorize_peer_uuid,
    session_core_t core,
    std::string expected_path,
    file_mapping::operations::execution_context_t operations_context,
    transport_config_t config,
    std::function<void()> on_close):
      ws_(std::move(stream)),
      validate_token_(std::move(validate_token)),
      authorize_peer_uuid_(std::move(authorize_peer_uuid)),
      core_(std::move(core)),
      expected_path_(std::move(expected_path)),
      operations_context_(std::move(operations_context)),
      config_(std::move(config)),
      on_close_(std::move(on_close)) {
  }

  beast_session_t::~beast_session_t() {
    if (on_close_) {
      on_close_();
    }
  }

  void
  beast_session_t::start() {
    ws_.read_message_max(std::max<std::uint64_t>(config_.max_control_frame_bytes, config_.max_binary_frame_bytes));
    ws_.next_layer().async_handshake(ssl::stream_base::server,
      [self = shared_from_this()](beast::error_code ec) {
        self->on_tls_handshake(ec);
      });
  }

  void
  beast_session_t::close() {
    close_requested_ = true;
    if (!write_active_) {
      close_with_error();
    }
  }

  void
  beast_session_t::on_tls_handshake(beast::error_code ec) {
    if (ec) {
      close_with_error();
      return;
    }

    http::async_read(ws_.next_layer(), read_buffer_, upgrade_request_,
      [self = shared_from_this()](beast::error_code read_ec, std::size_t bytes_transferred) {
        self->on_ws_request(read_ec, bytes_transferred);
      });
  }

  void
  beast_session_t::on_ws_request(beast::error_code ec, std::size_t /*bytes_transferred*/) {
    if (ec) {
      close_with_error();
      return;
    }
    if (!websocket::is_upgrade(upgrade_request_)) {
      close_with_error();
      return;
    }

    const auto target_result = validate_session_target(
      upgrade_request_.target(),
      expected_path_,
      validate_token_);
    if (!target_result.ok) {
      close_with_error();
      return;
    }

    core_ = session_core_t { "host", target_result.client_uuid, authorize_peer_uuid_, operations_context_ };
    ws_.async_accept(upgrade_request_, [self = shared_from_this()](beast::error_code accept_ec) {
      self->on_ws_accept(accept_ec);
    });
  }

  void
  beast_session_t::on_ws_accept(beast::error_code ec) {
    if (ec) {
      close_with_error();
      return;
    }

    read_next();
  }

  void
  beast_session_t::read_next() {
    if (close_requested_) {
      close_with_error();
      return;
    }

    ws_.async_read(read_buffer_,
      [self = shared_from_this()](beast::error_code ec, std::size_t bytes_transferred) {
        self->on_read(ec, bytes_transferred);
      });
  }

  void
  beast_session_t::on_read(beast::error_code ec, std::size_t /*bytes_transferred*/) {
    if (ec == websocket::error::closed) {
      close_requested_ = true;
      return;
    }
    if (ec) {
      close_with_error();
      return;
    }

    inbound_result_t result;
    if (ws_.got_text()) {
      if (read_buffer_.size() > config_.max_control_frame_bytes) {
        close_with_error();
        return;
      }
      result = core_.handle_text(beast::buffers_to_string(read_buffer_.data()));
    }
    else {
      if (read_buffer_.size() > config_.max_binary_frame_bytes) {
        close_with_error();
        return;
      }
      std::vector<std::uint8_t> bytes(read_buffer_.size());
      boost::asio::buffer_copy(boost::asio::buffer(bytes), read_buffer_.data());
      result = core_.handle_binary(bytes.data(), bytes.size());
    }
    read_buffer_.consume(read_buffer_.size());

    if (result.reply) {
      queue_reply(std::move(*result.reply));
    }

    if (!result.ok || result.close) {
      close_requested_ = true;
      if (!write_active_) {
        close_with_error();
      }
      return;
    }

    read_next();
  }

  void
  beast_session_t::queue_reply(outbound_frame_t frame) {
    if (write_queue_.size() >= config_.max_write_queue_frames) {
      close_requested_ = true;
      close_with_error();
      return;
    }

    write_queue_.emplace_back(std::move(frame));
    if (!write_active_) {
      write_next();
    }
  }

  void
  beast_session_t::write_next() {
    if (write_queue_.empty()) {
      write_active_ = false;
      if (close_requested_) {
        close_with_error();
      }
      return;
    }

    write_active_ = true;
    auto &front = write_queue_.front();
    ws_.text(front.kind == frame_kind_e::text);
    if (front.kind == frame_kind_e::text) {
      ws_.async_write(boost::asio::buffer(front.text),
        [self = shared_from_this()](beast::error_code ec, std::size_t bytes_transferred) {
          self->on_write(ec, bytes_transferred);
        });
    }
    else {
      ws_.async_write(boost::asio::buffer(front.binary),
        [self = shared_from_this()](beast::error_code ec, std::size_t bytes_transferred) {
          self->on_write(ec, bytes_transferred);
        });
    }
  }

  void
  beast_session_t::on_write(beast::error_code ec, std::size_t /*bytes_transferred*/) {
    if (ec) {
      close_with_error();
      return;
    }

    write_queue_.pop_front();
    write_next();
  }

  void
  beast_session_t::close_with_error() {
    if (socket_closed_) {
      return;
    }

    close_requested_ = true;
    socket_closed_ = true;
    beast::error_code ignored;
    ws_.next_layer().next_layer().shutdown(tcp::socket::shutdown_both, ignored);
    ws_.next_layer().next_layer().close(ignored);
  }
}  // namespace file_mapping_ws
