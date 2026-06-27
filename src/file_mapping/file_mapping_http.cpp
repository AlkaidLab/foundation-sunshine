/**
 * @file src/file_mapping_http.cpp
 * @brief HTTP route registration for directory mapping endpoints.
 */
#include "file_mapping_http.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "file_mapping_rpc.h"

namespace file_mapping_http {
  namespace {
    SimpleWeb::CaseInsensitiveMultimap
    json_headers() {
      return {
        { "Content-Type", "application/json" },
        { "Cache-Control", "no-store" },
        { "X-Content-Type-Options", "nosniff" }
      };
    }

    bool
    has_websocket_upgrade(const SimpleWeb::CaseInsensitiveMultimap &headers) {
      auto it = headers.find("upgrade");
      if (it == headers.end()) {
        return false;
      }

      std::string value = it->second;
      std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
      });
      return value == "websocket";
    }

    std::string
    header_value(const SimpleWeb::CaseInsensitiveMultimap &headers, const std::string &name) {
      auto it = headers.find(name);
      return it == headers.end() ? std::string {} : it->second;
    }

    capability_request_t
    make_capability_request(const req_https_t &req) {
      capability_request_t request;
      auto args = req->parse_query_string();
      if (auto it = args.find("client_uuid"); it != args.end()) {
        request.client_uuid = it->second;
      }
      if (request.client_uuid.empty()) {
        request.client_uuid = header_value(req->header, "x-file-mapping-client-uuid");
      }
      return request;
    }

    std::string
    url_without_query(std::string_view url) {
      const auto query_start = url.find('?');
      return std::string { query_start == std::string_view::npos ? url : url.substr(0, query_start) };
    }

    std::string
    make_request_session_url(const capability_state_t &state) {
      if (!state.session_url.empty()) {
        return url_without_query(state.session_url);
      }
      return {};
    }

    void
    write_response(resp_https_t resp, const http_response_t &out) {
      resp->write(out.status, out.body, out.headers);
    }

    capability_state_t
    get_state(const capability_state_fn &state_fn, const capability_request_t &request) {
      return state_fn ? state_fn(request) : capability_state_t {};
    }

    void
    handle_capability(const auth_fn &auth, const capability_state_fn &state_fn, resp_https_t resp, req_https_t req) {
      if (!auth(resp, req)) {
        return;
      }
      auto capability_request = make_capability_request(req);
      write_response(
        std::move(resp),
        make_capability_response(get_state(state_fn, capability_request), header_value(req->header, "host")));
    }

    void
    handle_session(const auth_fn &auth, resp_https_t resp, req_https_t req) {
      if (!auth(resp, req)) {
        return;
      }
      write_response(std::move(resp), make_session_placeholder_response(req->header));
    }
  }  // namespace

  http_response_t
  make_capability_response(const capability_state_t &state, std::string_view request_host) {
    (void) request_host;
    nlohmann::json body;
    body["ok"] = true;
    body["enabled"] = state.enabled;
    body["listening"] = state.listening;
    body["version"] = file_mapping::rpc::kProtocolVersion;
    body["transport"] = "wss";
    body["control"] = "json";
    body["data"] = "binary";
    body["session_endpoint"] = state.session_endpoint.empty() ? "/api/v1/file-mapping/session" : state.session_endpoint;
    body["session_url"] = make_request_session_url(state);
    body["session_token"] = state.session_token;
    body["port"] = state.port;
    if (!state.client_uuid.empty()) {
      body["client_uuid"] = state.client_uuid;
    }
    if (!state.error.empty()) {
      body["error"] = state.error;
    }
    body["diagnostics"] = nlohmann::json::object();
    for (const auto &[key, value] : state.diagnostics) {
      body["diagnostics"][key] = value;
    }
    body["features"] = nlohmann::json::array({
      "mappings",
      "transfer_jobs",
      "explicit_authorization",
      "cancel_job"
    });
    body["limits"] = {
      { "binary_header_size", file_mapping::rpc::kBinaryHeaderSize },
      { "max_protocol_version", file_mapping::rpc::kProtocolVersion }
    };

    return { SimpleWeb::StatusCode::success_ok, body.dump(), json_headers() };
  }

  http_response_t
  make_session_placeholder_response(const SimpleWeb::CaseInsensitiveMultimap &request_headers) {
    nlohmann::json body;
    body["ok"] = false;

    auto headers = json_headers();
    if (!has_websocket_upgrade(request_headers)) {
      body["error"] = "upgrade_required";
      headers.emplace("Upgrade", "websocket");
      return { SimpleWeb::StatusCode::client_error_upgrade_required, body.dump(), std::move(headers) };
    }

    body["error"] = "session_not_implemented";
    body["message"] = "file mapping websocket sessions are served by the advertised Beast endpoint";
    return { SimpleWeb::StatusCode::server_error_not_implemented, body.dump(), std::move(headers) };
  }

  void
  register_routes(https_server_t &server, auth_fn auth, capability_state_fn state) {
    auto auth_cap = std::make_shared<auth_fn>(std::move(auth));
    auto state_cap = std::make_shared<capability_state_fn>(std::move(state));

    server.resource["^/api/v1/file-mapping/capability$"]["GET"] =
      [auth_cap, state_cap](resp_https_t resp, req_https_t req) {
        handle_capability(*auth_cap, *state_cap, std::move(resp), std::move(req));
      };

    server.resource["^/api/v1/file-mapping/session$"]["GET"] =
      [auth_cap](resp_https_t resp, req_https_t req) {
        handle_session(*auth_cap, std::move(resp), std::move(req));
      };
  }
}  // namespace file_mapping_http
