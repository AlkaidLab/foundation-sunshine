/**
 * @file src/file_mapping/service.cpp
 * @brief Built-in feature service for Windows directory mapping.
 */
#include "service.h"

#include <utility>

#include "src/logging.h"

#include "file_mapping_config.h"
#include "file_mapping_operations.h"
#include "file_mapping_store.h"

using namespace std::literals;

namespace file_mapping {
  service_t::~service_t() {
    stop();
  }

  void
  service_t::start(config_t config) {
    stop();

    config_ = std::move(config);
    if (config_.bind_address.empty()) {
      config_.bind_address = "0.0.0.0";
    }

    auto config_result = file_mapping_config::parse_mappings_json(config_.mappings_json);
    for (const auto &config_warning : config_result.warnings) {
      BOOST_LOG(warning) << config_warning;
    }
    file_mapping_store::global().replace(std::move(config_result.mappings));

    auto operations_context = file_mapping::operations::execution_context_t {};
    operations_context.mapping_provider = []() {
      return file_mapping_store::global().snapshot();
    };

    auto token_store = std::make_shared<file_mapping_token::token_store_t>();
    file_mapping_ws::transport_config_t transport_config;
    transport_config.bind_address = config_.bind_address;
    transport_config.port = config_.port;
    transport_config.certificate_file = config_.certificate_file;
    transport_config.private_key_file = config_.private_key_file;
    // The capability endpoint is served by nvhttp HTTPS after paired-client
    // certificate verification. The Beast data port is additionally gated by
    // a short-lived token bound to the certificate-derived client UUID.
    transport_config.require_client_certificate = false;

    auto candidate = std::make_shared<file_mapping_ws::server_t>(
      io_,
      std::move(transport_config),
      [token_store](std::string_view token) {
        return token_store->consume(std::string { token });
      },
      config_.authorize_client,
      operations_context);

    stopping_ = false;
    if (auto result = candidate->start(); result.ok) {
      {
        std::scoped_lock lock { mutex_ };
        tokens_ = std::move(token_store);
        server_ = std::move(candidate);
        start_error_.clear();
      }
      thread_ = std::thread([this]() {
        try {
          io_.run();
        }
        catch (const std::exception &err) {
          if (!stopping_) {
            BOOST_LOG(error) << "File mapping WebSocket server failed: "sv << err.what();
          }
        }
      });
      BOOST_LOG(info) << "File mapping WebSocket server listening on port ["sv << server_->bound_port() << "]"sv;
    }
    else {
      std::scoped_lock lock { mutex_ };
      start_error_ = result.error;
      BOOST_LOG(warning) << "File mapping WebSocket server disabled: "sv << result.error;
    }
  }

  void
  service_t::stop() {
    stopping_ = true;
    {
      std::scoped_lock lock { mutex_ };
      if (server_) {
        server_->stop();
      }
      server_.reset();
      tokens_.reset();
    }

    io_.stop();
    if (thread_.joinable()) {
      thread_.join();
    }
    io_.restart();
  }

  file_mapping_http::http_response_t
  service_t::make_capability_response(std::string client_uuid, std::string_view request_host) {
    return file_mapping_http::make_capability_response(make_capability_state(std::move(client_uuid)), request_host);
  }

  file_mapping_http::capability_state_t
  service_t::make_capability_state(std::string client_uuid) {
    file_mapping_http::capability_state_t state;
    state.enabled = true;
    state.session_endpoint = "/api/v1/file-mapping/session";
    state.client_uuid = std::move(client_uuid);
    state.diagnostics["bind_address"] = config_.bind_address.empty() ? "0.0.0.0" : config_.bind_address;
    state.diagnostics["configured_port"] = std::to_string(config_.port);

    std::shared_ptr<file_mapping_token::token_store_t> tokens;
    {
      std::scoped_lock lock { mutex_ };
      state.error = start_error_;
      if (!start_error_.empty()) {
        state.diagnostics["start_error"] = start_error_;
      }
      if (server_) {
        state.listening = server_->state() == file_mapping_ws::transport_state_e::listening;
        state.port = server_->bound_port();
      }
      tokens = tokens_;
    }

    state.diagnostics["listening"] = state.listening ? "true" : "false";
    state.diagnostics["bound_port"] = std::to_string(state.port);
    state.diagnostics["client_certificate"] = state.client_uuid.empty() ? "missing" : "verified";

    if (state.client_uuid.empty()) {
      state.error = "paired client certificate is required for file mapping";
      state.diagnostics["token"] = "not_issued";
    }
    else if (state.listening && state.port != 0 && tokens) {
      state.session_token = tokens->issue(state.client_uuid);
      state.diagnostics["token"] = state.session_token.empty() ? "rate_limited" : "issued";
      if (state.session_token.empty() && state.error.empty()) {
        state.error = "file mapping session token rate limited";
      }
    }
    else {
      state.diagnostics["token"] = "not_issued";
    }

    return state;
  }
}  // namespace file_mapping
