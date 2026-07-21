/**
 * @file src/file_mapping/service.h
 * @brief Built-in feature service for Windows directory mapping.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include <boost/asio/io_context.hpp>

#include "file_mapping_http.h"
#include "file_mapping_token.h"
#include "file_mapping_ws_server.h"

namespace file_mapping {
  class service_t {
  public:
    using client_authorizer_fn = file_mapping_ws::client_uuid_authorizer_t;

    struct config_t {
      std::string bind_address = "0.0.0.0";
      std::uint16_t port = 0;
      std::string certificate_file;
      std::string private_key_file;
      std::string mappings_json;
      client_authorizer_fn authorize_client;
    };

    service_t() = default;
    ~service_t();

    service_t(const service_t &) = delete;
    service_t &operator=(const service_t &) = delete;

    void start(config_t config);
    void stop();

    file_mapping_http::http_response_t make_capability_response(
      std::string client_uuid,
      std::string_view request_host);

  private:
    file_mapping_http::capability_state_t make_capability_state(std::string client_uuid);

    mutable std::mutex mutex_;
    boost::asio::io_context io_;
    std::shared_ptr<file_mapping_ws::server_t> server_;
    std::shared_ptr<file_mapping_token::token_store_t> tokens_;
    std::thread thread_;
    config_t config_;
    std::string start_error_;
    std::atomic_bool stopping_ { false };
  };
}  // namespace file_mapping
