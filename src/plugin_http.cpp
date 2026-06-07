/**
 * @file src/plugin_http.cpp
 * @brief Web UI endpoint definitions for lifecycle plugin management.
 */

#include "plugin_http.h"

#include <sstream>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "logging.h"
#include "plugin.h"

namespace plugin_http {
  using namespace std::literals;

  namespace {

    SimpleWeb::CaseInsensitiveMultimap
    json_headers() {
      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "application/json");
      return headers;
    }

    void
    write_json(resp_https_t response, SimpleWeb::StatusCode status, const nlohmann::json &body) {
      response->write(status, body.dump(), json_headers());
      response->close_connection_after_response = true;
    }

    nlohmann::json
    read_body_json(const req_https_t &request) {
      std::stringstream stream;
      stream << request->content.rdbuf();
      const auto body = stream.str();
      if (body.empty()) {
        return nlohmann::json::object();
      }

      return nlohmann::json::parse(body);
    }

    std::string
    plugin_id_from_match(const req_https_t &request) {
      return request->path_match.size() >= 2 ? request->path_match[1].str() : std::string {};
    }

    std::string
    action_id_from_match(const req_https_t &request) {
      return request->path_match.size() >= 3 ? request->path_match[2].str() : std::string {};
    }

    void
    list_plugins(const auth_fn &auth, resp_https_t response, req_https_t request) {
      if (!auth(response, request)) {
        return;
      }

      write_json(response, SimpleWeb::StatusCode::success_ok, plugin::list_installed_plugins());
    }

    void
    list_marketplace(const auth_fn &auth, resp_https_t response, req_https_t request) {
      if (!auth(response, request)) {
        return;
      }

      std::string error;
      auto marketplace = plugin::list_marketplace_plugins(error);
      if (!error.empty()) {
        write_json(response, SimpleWeb::StatusCode::server_error_bad_gateway,
          { { "status", false }, { "error", error } });
        return;
      }

      write_json(response, SimpleWeb::StatusCode::success_ok, marketplace);
    }

    void
    set_enabled(const auth_fn &auth, resp_https_t response, req_https_t request) {
      if (!auth(response, request)) {
        return;
      }

      try {
        const auto body = read_body_json(request);
        if (!body.contains("enabled") || !body.at("enabled").is_boolean()) {
          write_json(response, SimpleWeb::StatusCode::client_error_bad_request,
            { { "status", false }, { "error", "missing boolean field: enabled" } });
          return;
        }

        std::string error;
        const auto id = plugin_id_from_match(request);
        const auto enabled = body.at("enabled").get<bool>();
        if (!plugin::set_plugin_enabled(id, enabled, error)) {
          write_json(response, SimpleWeb::StatusCode::client_error_bad_request,
            { { "status", false }, { "error", error } });
          return;
        }

        write_json(response, SimpleWeb::StatusCode::success_ok,
          { { "status", true }, { "id", id }, { "enabled", enabled } });
      }
      catch (const std::exception &err) {
        BOOST_LOG(warning) << "Plugin enable endpoint failed: "sv << err.what();
        write_json(response, SimpleWeb::StatusCode::client_error_bad_request,
          { { "status", false }, { "error", err.what() } });
      }
    }

    void
    save_config(const auth_fn &auth, resp_https_t response, req_https_t request) {
      if (!auth(response, request)) {
        return;
      }

      try {
        const auto body = read_body_json(request);
        const auto config = body.contains("config") && body.at("config").is_object() ? body.at("config") : body;
        if (!config.is_object()) {
          write_json(response, SimpleWeb::StatusCode::client_error_bad_request,
            { { "status", false }, { "error", "plugin config must be an object" } });
          return;
        }

        std::string error;
        const auto id = plugin_id_from_match(request);
        if (!plugin::save_plugin_config(id, config, error)) {
          write_json(response, SimpleWeb::StatusCode::client_error_bad_request,
            { { "status", false }, { "error", error } });
          return;
        }

        write_json(response, SimpleWeb::StatusCode::success_ok,
          { { "status", true }, { "id", id }, { "config", config } });
      }
      catch (const std::exception &err) {
        BOOST_LOG(warning) << "Plugin config endpoint failed: "sv << err.what();
        write_json(response, SimpleWeb::StatusCode::client_error_bad_request,
          { { "status", false }, { "error", err.what() } });
      }
    }

    void
    run_action(const auth_fn &auth, resp_https_t response, req_https_t request) {
      if (!auth(response, request)) {
        return;
      }

      try {
        std::string error;
        nlohmann::json result;
        const auto id = plugin_id_from_match(request);
        const auto action_id = action_id_from_match(request);
        if (!plugin::run_plugin_action(id, action_id, result, error)) {
          write_json(response, SimpleWeb::StatusCode::client_error_bad_request,
            { { "status", false }, { "error", error } });
          return;
        }

        write_json(response, SimpleWeb::StatusCode::success_ok,
          { { "status", result.value("success", false) }, { "id", id }, { "action_id", action_id }, { "result", result } });
      }
      catch (const std::exception &err) {
        BOOST_LOG(warning) << "Plugin action endpoint failed: "sv << err.what();
        write_json(response, SimpleWeb::StatusCode::client_error_bad_request,
          { { "status", false }, { "error", err.what() } });
      }
    }

  }  // namespace

  void
  register_routes(https_server_t &server, auth_fn auth) {
    server.resource["^/api/plugins$"]["GET"] = [auth](resp_https_t response, req_https_t request) {
      list_plugins(auth, std::move(response), std::move(request));
    };

    server.resource["^/api/plugins/marketplace$"]["GET"] = [auth](resp_https_t response, req_https_t request) {
      list_marketplace(auth, std::move(response), std::move(request));
    };

    server.resource["^/api/plugins/([^/]+)/enabled$"]["POST"] = [auth](resp_https_t response, req_https_t request) {
      set_enabled(auth, std::move(response), std::move(request));
    };

    server.resource["^/api/plugins/([^/]+)/config$"]["POST"] = [auth](resp_https_t response, req_https_t request) {
      save_config(auth, std::move(response), std::move(request));
    };

    server.resource["^/api/plugins/([^/]+)/actions/([^/]+)$"]["POST"] = [auth](resp_https_t response, req_https_t request) {
      run_action(auth, std::move(response), std::move(request));
    };
  }

}  // namespace plugin_http
