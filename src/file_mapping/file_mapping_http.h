/**
 * @file src/file_mapping_http.h
 * @brief HTTP route registration for directory mapping endpoints.
 */
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <cstdint>

#include <Simple-Web-Server/server_https.hpp>

namespace file_mapping_http {
  using https_server_t = SimpleWeb::Server<SimpleWeb::HTTPS>;
  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request>;
  using auth_fn = std::function<bool(resp_https_t, req_https_t)>;

  struct capability_state_t {
    bool enabled = false;
    bool listening = false;
    std::string session_endpoint = "/api/v1/file-mapping/session";
    std::string session_url;
    std::string session_token;
    std::string error;
    std::string client_uuid;
    std::map<std::string, std::string> diagnostics;
    std::uint16_t port = 0;
  };

  struct capability_request_t {
    std::string client_uuid;
  };
  using capability_state_fn = std::function<capability_state_t(const capability_request_t &)>;

  struct http_response_t {
    SimpleWeb::StatusCode status;
    std::string body;
    SimpleWeb::CaseInsensitiveMultimap headers;
  };

  /// Build the JSON capability response for file mapping clients.
  http_response_t make_capability_response(const capability_state_t &state = {}, std::string_view request_host = {});

  /// Build the current response for the WSS session endpoint.
  ///
  /// The config HTTPS server does not own the WebSocket data channel. Clients
  /// should use the Beast endpoint advertised by make_capability_response().
  http_response_t make_session_placeholder_response(const SimpleWeb::CaseInsensitiveMultimap &request_headers);

  /// Register /api/v1/file-mapping/* routes on the config HTTPS server.
  void register_routes(https_server_t &server, auth_fn auth, capability_state_fn state = {});
}  // namespace file_mapping_http
