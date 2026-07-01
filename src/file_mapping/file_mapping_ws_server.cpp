/**
 * @file src/file_mapping_ws_server.cpp
 * @brief Boost.Asio listener for the file mapping WebSocket transport.
 */
#include "file_mapping_ws_server.h"

#include <utility>

#ifdef _WIN32
  #include <winsock2.h>
#endif

#include <boost/asio/ip/address.hpp>

#include "file_mapping_ws_session.h"

namespace file_mapping_ws {
  namespace {
    validation_result_t
    fail(std::string error) {
      return { false, std::move(error) };
    }
  }  // namespace

  server_t::server_t(
    asio::io_context &io,
    transport_config_t config,
    session_token_validator_t validate_token,
    client_uuid_authorizer_t authorize_peer_uuid,
    file_mapping::operations::execution_context_t operations_context):
      io_(io),
      config_(std::move(config)),
      validate_token_(std::move(validate_token)),
      authorize_peer_uuid_(std::move(authorize_peer_uuid)),
      operations_context_(std::move(operations_context)),
      ssl_ctx_(ssl::context::tls_server),
      acceptor_(io) {
  }

  validation_result_t
  server_t::start() {
    if (state_.load() != transport_state_e::stopped) {
      return fail("file mapping websocket server is already started");
    }
    if (weak_from_this().expired()) {
      return fail("file mapping websocket server must be owned by std::shared_ptr before start");
    }

    state_.store(transport_state_e::starting);
    if (auto config_result = validate_config(config_); !config_result.ok) {
      state_.store(transport_state_e::stopped);
      return config_result;
    }
    if (auto ssl_result = configure_ssl(); !ssl_result.ok) {
      state_.store(transport_state_e::stopped);
      return ssl_result;
    }
    if (auto acceptor_result = open_acceptor(); !acceptor_result.ok) {
      state_.store(transport_state_e::stopped);
      return acceptor_result;
    }

    state_.store(transport_state_e::listening);
    accept_next();
    return { true, {} };
  }

  void
  server_t::stop() {
    state_.store(transport_state_e::stopping);
    boost::system::error_code ignored;
    acceptor_.cancel(ignored);
    acceptor_.close(ignored);
    state_.store(transport_state_e::stopped);
  }

  transport_state_e
  server_t::state() const {
    return state_.load();
  }

  std::uint16_t
  server_t::bound_port() const {
    if (!acceptor_.is_open()) {
      return 0;
    }

    boost::system::error_code ec;
    auto endpoint = acceptor_.local_endpoint(ec);
    return ec ? 0 : endpoint.port();
  }

  validation_result_t
  server_t::configure_ssl() {
    boost::system::error_code ec;
    ssl_ctx_.set_options(
      ssl::context::default_workarounds |
      ssl::context::no_sslv2 |
      ssl::context::no_sslv3 |
      ssl::context::no_tlsv1 |
      ssl::context::no_tlsv1_1 |
      ssl::context::single_dh_use,
      ec);
    if (ec) {
      return fail(ec.message());
    }

    ssl_ctx_.use_certificate_chain_file(config_.certificate_file, ec);
    if (ec) {
      return fail(ec.message());
    }

    ssl_ctx_.use_private_key_file(config_.private_key_file, ssl::context::pem, ec);
    if (ec) {
      return fail(ec.message());
    }

    if (config_.require_client_certificate) {
      ssl_ctx_.set_verify_mode(ssl::verify_peer | ssl::verify_fail_if_no_peer_cert, ec);
      if (ec) {
        return fail(ec.message());
      }
    }

    return { true, {} };
  }

  validation_result_t
  server_t::open_acceptor() {
    boost::system::error_code ec;
    const auto fail_and_close = [this](const boost::system::error_code &error) {
      boost::system::error_code ignored;
      acceptor_.close(ignored);
      return fail(error.message());
    };

    const auto address = boost::asio::ip::make_address(config_.bind_address, ec);
    if (ec) {
      return fail(ec.message());
    }

    tcp::endpoint endpoint { address, config_.port };
    acceptor_.open(endpoint.protocol(), ec);
    if (ec) {
      return fail_and_close(ec);
    }

#ifdef _WIN32
    {
      BOOL exclusive = TRUE;
      if (::setsockopt(
            acceptor_.native_handle(),
            SOL_SOCKET,
            SO_EXCLUSIVEADDRUSE,
            reinterpret_cast<const char *>(&exclusive),
            sizeof(exclusive)) == SOCKET_ERROR) {
        return fail_and_close({ WSAGetLastError(), boost::asio::error::get_system_category() });
      }
    }
#else
    acceptor_.set_option(tcp::acceptor::reuse_address(true), ec);
    if (ec) {
      return fail_and_close(ec);
    }
#endif

    acceptor_.bind(endpoint, ec);
    if (ec) {
      return fail_and_close(ec);
    }

    acceptor_.listen(boost::asio::socket_base::max_listen_connections, ec);
    if (ec) {
      return fail_and_close(ec);
    }

    return { true, {} };
  }

  void
  server_t::accept_next() {
    if (state_.load() != transport_state_e::listening) {
      return;
    }

    acceptor_.async_accept([self = shared_from_this()](boost::system::error_code ec, tcp::socket socket) {
      self->on_accept(ec, std::move(socket));
    });
  }

  void
  server_t::on_accept(boost::system::error_code ec, tcp::socket socket) {
    if (state_.load() != transport_state_e::listening) {
      return;
    }

    if (!ec) {
      if (config_.max_active_sessions != 0 && active_sessions_.load() >= config_.max_active_sessions) {
        boost::system::error_code ignored;
        socket.shutdown(tcp::socket::shutdown_both, ignored);
        socket.close(ignored);
        accept_next();
        return;
      }

      active_sessions_.fetch_add(1);
      websocket_stream_t ws { std::move(socket), ssl_ctx_ };
      auto self = shared_from_this();
      std::make_shared<beast_session_t>(
        std::move(ws),
        validate_token_,
        authorize_peer_uuid_,
        session_core_t {},
        "/api/v1/file-mapping/session",
        operations_context_,
        config_,
        [self]() {
          self->active_sessions_.fetch_sub(1);
        })
        ->start();
    }

    accept_next();
  }
}  // namespace file_mapping_ws
