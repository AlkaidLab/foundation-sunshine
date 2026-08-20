/**
 * @file src/webhook/webhook_api.cpp
 * @brief Business handlers for standalone Webhook configuration and testing.
 */

#include "webhook_api.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "src/logging.h"
#include "webhook.h"
#include "webhook_auth.h"

namespace webhook::api {
  namespace {
    using json = nlohmann::json;

    constexpr std::size_t MAX_REQUEST_SIZE = 64 * 1024;
    std::mutex configuration_transaction_mutex;

    SimpleWeb::CaseInsensitiveMultimap
    json_headers() {
      return {
        {"Content-Type", "application/json"},
        {"Cache-Control", "no-store"},
        {"X-Content-Type-Options", "nosniff"},
        {"X-Frame-Options", "DENY"},
        {"Content-Security-Policy", "frame-ancestors 'none';"},
      };
    }

    void
    write_json(resp_https_t response, SimpleWeb::StatusCode status, const json &body) {
      response->write(status, body.dump(), json_headers());
    }

    void
    write_error(
      resp_https_t response,
      SimpleWeb::StatusCode status,
      const char *message,
      const char *error_code = nullptr
    ) {
      json body {
        {"status", false},
        {"error", message},
      };
      if (error_code) {
        body["error_code"] = error_code;
      }
      write_json(std::move(response), status, body);
    }

    void
    write_unhandled_error(resp_https_t response) noexcept {
      if (!response) {
        return;
      }
      try {
        write_error(
          std::move(response),
          SimpleWeb::StatusCode::server_error_internal_server_error,
          "Webhook request failed"
        );
      }
      catch (...) {
        // A disconnected UI client must not escape the Webhook route boundary.
      }
    }

    bool
    parse_configuration(const json &input, auth::settings_t &settings) {
      if (!input.is_object() || input.size() != 5 ||
          !input.contains("webhook_enabled") || !input["webhook_enabled"].is_boolean() ||
          !input.contains("webhook_url") || !input["webhook_url"].is_string() ||
          !input.contains("webhook_skip_ssl_verify") || !input["webhook_skip_ssl_verify"].is_boolean() ||
          !input.contains("webhook_timeout") || !input["webhook_timeout"].is_number_integer() ||
          !input.contains("webhook_events") || !input["webhook_events"].is_string()) {
        return false;
      }

      const auto timeout = input["webhook_timeout"].get<std::int64_t>();
      const auto &url = input["webhook_url"].get_ref<const std::string &>();
      if (url.size() > MAX_URL_SIZE ||
          timeout < MIN_TIMEOUT.count() ||
          timeout > MAX_TIMEOUT.count()) {
        return false;
      }

      std::vector<int> events;
      if (!auth::parse_event_ids(input["webhook_events"].get_ref<const std::string &>(), events)) {
        return false;
      }

      settings.enabled = input["webhook_enabled"].get<bool>();
      settings.url = url;
      settings.skip_ssl_verify = input["webhook_skip_ssl_verify"].get<bool>();
      settings.timeout = std::chrono::milliseconds {timeout};
      settings.events = std::move(events);
      return validate_configuration(settings);
    }

    bool
    read_json_body(const resp_https_t &response, const req_https_t &request, json &body) {
      if (request->content.size() > MAX_REQUEST_SIZE) {
        write_error(response, SimpleWeb::StatusCode::client_error_payload_too_large, "Request body is too large");
        return false;
      }
      const auto content = request->content.string();

      body = json::parse(content, nullptr, false);
      if (body.is_discarded()) {
        write_error(response, SimpleWeb::StatusCode::client_error_bad_request, "Invalid JSON");
        return false;
      }
      return true;
    }

    void
    get_config_impl(resp_https_t response, const std::filesystem::path &config_path) {
      auth::load_result_t result {
        auth::load_status_t::INVALID,
        {},
      };
      try {
        std::lock_guard<std::mutex> lock(configuration_transaction_mutex);
        result = auth::load(config_path);
      }
      catch (...) {
        // The generic response below keeps local paths and file contents private.
      }
      if (result.status == auth::load_status_t::INVALID) {
        write_error(
          std::move(response),
          SimpleWeb::StatusCode::server_error_internal_server_error,
          "Failed to read Webhook configuration",
          "webhook_config_invalid"
        );
        return;
      }

      write_json(
        std::move(response),
        SimpleWeb::StatusCode::success_ok,
        {
          {"status", true},
          {"webhook_enabled", result.settings.enabled},
          {"webhook_events", auth::serialize_event_ids(result.settings.events)},
          {"webhook_url", result.settings.url},
          {"webhook_skip_ssl_verify", result.settings.skip_ssl_verify},
          {"webhook_timeout", result.settings.timeout.count()},
        }
      );
    }

    void
    save_config_impl(
      resp_https_t response,
      req_https_t request,
      const std::filesystem::path &config_path
    ) {
      json input;
      if (!read_json_body(response, request, input)) {
        return;
      }

      auth::settings_t settings;
      try {
        if (!parse_configuration(input, settings)) {
          write_error(response, SimpleWeb::StatusCode::client_error_bad_request, "Invalid Webhook configuration");
          return;
        }
      }
      catch (...) {
        write_error(response, SimpleWeb::StatusCode::client_error_bad_request, "Invalid Webhook configuration");
        return;
      }

      bool saved = false;
      bool applied = false;
      bool prepared_ok = false;
      try {
        std::lock_guard<std::mutex> lock(configuration_transaction_mutex);
        auto prepared = prepare_configuration(settings);
        prepared_ok = static_cast<bool>(prepared);
        if (prepared) {
          try {
            saved = auth::save(config_path, prepared.value());
          }
          catch (...) {
            saved = false;
          }
          if (saved) {
            applied = commit_configuration(std::move(prepared));
          }
        }
      }
      catch (...) {
        // Return a stable error without exposing the selected local path.
      }
      if (!prepared_ok) {
        write_error(response, SimpleWeb::StatusCode::server_error_internal_server_error, "Failed to apply Webhook configuration");
        return;
      }
      if (!saved) {
        write_error(response, SimpleWeb::StatusCode::server_error_internal_server_error, "Failed to write Webhook configuration");
        return;
      }
      if (!applied) {
        BOOST_LOG(error) << "Webhook configuration was persisted but could not be published to the runtime";
        write_error(response, SimpleWeb::StatusCode::server_error_internal_server_error, "Failed to apply Webhook configuration");
        return;
      }

      // A failed startup is isolated from Sunshine and can be retried by an
      // explicit user save. Report the runtime state separately: persistence
      // success must not be presented as immediate delivery availability.
      const bool runtime_available = ensure_running();
      BOOST_LOG(info) << "Webhook configuration saved successfully; runtime is "
                      << (runtime_available ? "available" : "unavailable");
      write_json(
        std::move(response),
        SimpleWeb::StatusCode::success_ok,
        {
          {"status", true},
          {"runtime_active", runtime_available},
        }
      );
    }

    void
    test_delivery_impl(resp_https_t response, req_https_t request) {
      json input;
      if (!read_json_body(response, request, input)) {
        return;
      }
      if (!input.is_object() || (input.size() != 3 && input.size() != 4) ||
          !input.contains("webhook_url") || !input["webhook_url"].is_string() ||
          !input.contains("webhook_skip_ssl_verify") || !input["webhook_skip_ssl_verify"].is_boolean() ||
          !input.contains("webhook_timeout") || !input["webhook_timeout"].is_number_integer() ||
          (input.contains("webhook_retries") && !input["webhook_retries"].is_number_integer())) {
        write_error(response, SimpleWeb::StatusCode::client_error_bad_request, "Invalid Webhook test request");
        return;
      }

      settings_t settings;
      int retry_count = 0;
      try {
        settings.url = input["webhook_url"].get<std::string>();
        settings.skip_ssl_verify = input["webhook_skip_ssl_verify"].get<bool>();
        const auto timeout = input["webhook_timeout"].get<std::int64_t>();
        retry_count = input.value("webhook_retries", 0);
        if (settings.url.empty() || settings.url.size() > MAX_URL_SIZE ||
            timeout < MIN_TIMEOUT.count() ||
            timeout > MAX_TIMEOUT.count() ||
            retry_count < 0 ||
            retry_count > MAX_TEST_RETRIES) {
          write_error(response, SimpleWeb::StatusCode::client_error_bad_request, "Webhook URL, timeout, or retry count is out of range");
          return;
        }
        settings.timeout = std::chrono::milliseconds {timeout};
        if (!validate_configuration({
              true,
              settings.url,
              settings.skip_ssl_verify,
              settings.timeout,
              {},
            })) {
          write_error(response, SimpleWeb::StatusCode::client_error_bad_request, "Invalid Webhook URL");
          return;
        }
      }
      catch (...) {
        write_error(response, SimpleWeb::StatusCode::client_error_bad_request, "Invalid Webhook test request");
        return;
      }

      send_test_async(settings, retry_count, [response](delivery_result_t result) {
        write_json(
          response,
          SimpleWeb::StatusCode::success_ok,
          {
            {"status", result.success},
            {"http_status", result.http_status},
            {"attempts", result.attempts},
            {"error", delivery_error_name(result.error)},
          }
        );
      });
    }
  }  // namespace

  void
  get_config(resp_https_t response, const std::string &sunshine_config_file) noexcept {
    try {
      get_config_impl(response, auth::path_for(sunshine_config_file));
    }
    catch (...) {
      write_unhandled_error(std::move(response));
    }
  }

  void
  save_config(
    resp_https_t response,
    req_https_t request,
    const std::string &sunshine_config_file
  ) noexcept {
    try {
      save_config_impl(response, std::move(request), auth::path_for(sunshine_config_file));
    }
    catch (...) {
      write_unhandled_error(std::move(response));
    }
  }

  void
  test_delivery(resp_https_t response, req_https_t request) noexcept {
    try {
      test_delivery_impl(response, std::move(request));
    }
    catch (...) {
      write_unhandled_error(std::move(response));
    }
  }
}  // namespace webhook::api
