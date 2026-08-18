/**
 * @file src/ds5_config_api.cpp
 * @brief Authenticated HTTP handlers for independent DualSense settings.
 */

#include "ds5_config_api.h"

#include <cstddef>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <utility>

#include <nlohmann/json.hpp>

#include "ds5_config.h"
#include "logging.h"

namespace ds5_config::api {
  namespace {
    using json = nlohmann::json;
    constexpr std::size_t MAX_REQUEST_SIZE = 64 * 1024;
    std::mutex settings_transaction_mutex;

    SimpleWeb::CaseInsensitiveMultimap json_headers() {
      return {
        {"Content-Type", "application/json"},
        {"Cache-Control", "no-store"},
        {"X-Content-Type-Options", "nosniff"},
        {"X-Frame-Options", "DENY"},
        {"Content-Security-Policy", "frame-ancestors 'none';"},
      };
    }

    void write_json(resp_https_t response, SimpleWeb::StatusCode status, const json &body) {
      response->write(status, body.dump(), json_headers());
    }

    void write_error(
      resp_https_t response,
      SimpleWeb::StatusCode status,
      const char *message,
      const char *error_code = nullptr
    ) {
      json body {{"status", false}, {"error", message}};
      if (error_code) body["error_code"] = error_code;
      write_json(std::move(response), status, body);
    }

    json settings_json(
      const settings_t &settings,
      bool persisted,
      std::optional<bool> changed = std::nullopt
    ) {
      json result {
        {"status", true},
        {"applied", true},
        {"persisted", persisted},
        {"revision", settings.revision},
        {"ds5_enabled", settings.enabled},
        {"ds5_audio_haptics", settings.audio_haptics},
        {"ds5_legacy_haptics_strength", settings.legacy_strength},
        {"ds5_legacy_haptics_curve", settings.legacy_curve},
        {"ds5_legacy_haptics_noise_gate", settings.legacy_noise_gate},
        {"ds5_legacy_haptics_schema", 2},
        {"ds5_legacy_haptics_profile", legacy_profile_name(settings.legacy_profile)},
        {"ds5_legacy_haptics_max_output", settings.legacy_max_output},
        {"ds5_legacy_haptics_high_scale", settings.legacy_high_scale},
        {"ds5_legacy_haptics_response", legacy_response_name(settings.legacy_response)},
        {"ds5_legacy_haptics_body_mix", settings.legacy_body_mix},
      };
      if (changed) result["changed"] = *changed;
      return result;
    }

    bool same_values(const settings_t &left, const settings_t &right) noexcept {
      return left.enabled == right.enabled &&
             left.audio_haptics == right.audio_haptics &&
             left.legacy_strength == right.legacy_strength &&
             left.legacy_curve == right.legacy_curve &&
             left.legacy_noise_gate == right.legacy_noise_gate &&
             left.legacy_profile == right.legacy_profile &&
             left.legacy_max_output == right.legacy_max_output &&
             left.legacy_high_scale == right.legacy_high_scale &&
             left.legacy_response == right.legacy_response &&
             left.legacy_body_mix == right.legacy_body_mix;
    }

    bool parse_settings(const json &input, settings_t &settings) {
      const bool has_extended = input.is_object() && input.size() == 11 &&
                                input.contains("ds5_legacy_haptics_schema") &&
                                input["ds5_legacy_haptics_schema"].is_number_integer() &&
                                input["ds5_legacy_haptics_schema"].get<int>() == 2;
      if (!input.is_object() || (input.size() != 5 && !has_extended) ||
          !input.contains("ds5_enabled") || !input["ds5_enabled"].is_boolean() ||
          !input.contains("ds5_audio_haptics") || !input["ds5_audio_haptics"].is_boolean() ||
          !input.contains("ds5_legacy_haptics_strength") || !input["ds5_legacy_haptics_strength"].is_number() ||
          !input.contains("ds5_legacy_haptics_curve") || !input["ds5_legacy_haptics_curve"].is_number() ||
          !input.contains("ds5_legacy_haptics_noise_gate") || !input["ds5_legacy_haptics_noise_gate"].is_number() ||
          (has_extended && (!input.contains("ds5_legacy_haptics_profile") || !input["ds5_legacy_haptics_profile"].is_string() ||
                            !input.contains("ds5_legacy_haptics_max_output") || !input["ds5_legacy_haptics_max_output"].is_number() ||
                            !input.contains("ds5_legacy_haptics_high_scale") || !input["ds5_legacy_haptics_high_scale"].is_number() ||
                            !input.contains("ds5_legacy_haptics_response") || !input["ds5_legacy_haptics_response"].is_string() ||
                            !input.contains("ds5_legacy_haptics_body_mix") || !input["ds5_legacy_haptics_body_mix"].is_number()))) {
        return false;
      }
      settings = {
        input["ds5_enabled"].get<bool>(),
        input["ds5_audio_haptics"].get<bool>(),
        input["ds5_legacy_haptics_strength"].get<double>(),
        input["ds5_legacy_haptics_curve"].get<double>(),
        input["ds5_legacy_haptics_noise_gate"].get<double>(),
      };
      if (has_extended) {
        if (!parse_legacy_profile(input["ds5_legacy_haptics_profile"].get<std::string>(), settings.legacy_profile) ||
            !parse_legacy_response(input["ds5_legacy_haptics_response"].get<std::string>(), settings.legacy_response)) {
          return false;
        }
        settings.legacy_max_output = input["ds5_legacy_haptics_max_output"].get<double>();
        settings.legacy_high_scale = input["ds5_legacy_haptics_high_scale"].get<double>();
        settings.legacy_body_mix = input["ds5_legacy_haptics_body_mix"].get<double>();
      }
      return validate(settings);
    }

    void get_config_impl(resp_https_t response, const std::filesystem::path &path) {
      load_result_t result;
      {
        std::lock_guard<std::mutex> lock(settings_transaction_mutex);
        result = load(path);
      }
      if (result.status == load_status_t::INVALID) {
        write_error(
          std::move(response),
          SimpleWeb::StatusCode::server_error_internal_server_error,
          "Failed to read DualSense configuration",
          "ds5_config_invalid"
        );
        return;
      }
      const auto active = current();
      write_json(
        std::move(response),
        SimpleWeb::StatusCode::success_ok,
        settings_json(active, result.status == load_status_t::LOADED)
      );
    }

    void save_config_impl(resp_https_t response, req_https_t request, const std::filesystem::path &path) {
      if (request->content.size() > MAX_REQUEST_SIZE) {
        write_error(std::move(response), SimpleWeb::StatusCode::client_error_payload_too_large, "Request body is too large");
        return;
      }
      const auto input = json::parse(request->content.string(), nullptr, false);
      settings_t settings;
      if (input.is_discarded() || !parse_settings(input, settings)) {
        write_error(std::move(response), SimpleWeb::StatusCode::client_error_bad_request, "Invalid DualSense configuration");
        return;
      }

      bool saved = false;
      bool applied = false;
      bool changed = false;
      {
        std::lock_guard<std::mutex> lock(settings_transaction_mutex);
        const auto active = current();
        changed = !same_values(active, settings);
        settings.revision = !changed ? active.revision :
                              active.revision == (std::numeric_limits<std::uint64_t>::max)() ?
                                1 : active.revision + 1;
        auto prepared = prepare(settings);
        if (prepared) {
          saved = save(path, prepared.value());
          if (saved) applied = commit(std::move(prepared));
        }
      }
      if (!saved) {
        write_error(std::move(response), SimpleWeb::StatusCode::server_error_internal_server_error, "Failed to write DualSense configuration");
        return;
      }
      if (!applied) {
        BOOST_LOG(error) << "DualSense configuration was persisted but could not be published to the runtime";
        write_error(std::move(response), SimpleWeb::StatusCode::server_error_internal_server_error, "Failed to apply DualSense configuration");
        return;
      }

      if (changed) {
        BOOST_LOG(info) << "DualSense configuration saved and applied at revision " << settings.revision;
      }
      else {
        BOOST_LOG(debug) << "DualSense configuration save kept revision " << settings.revision;
      }
      write_json(
        std::move(response),
        SimpleWeb::StatusCode::success_ok,
        settings_json(settings, true, changed)
      );
    }

    void write_unhandled_error(resp_https_t response) noexcept {
      if (!response) return;
      try {
        write_error(std::move(response), SimpleWeb::StatusCode::server_error_internal_server_error, "DualSense request failed");
      }
      catch (...) {
      }
    }
  }  // namespace

  void get_config(resp_https_t response, const std::string &sunshine_config_file) noexcept {
    try {
      get_config_impl(response, path_for(sunshine_config_file));
    }
    catch (...) {
      write_unhandled_error(std::move(response));
    }
  }

  void save_config(
    resp_https_t response,
    req_https_t request,
    const std::string &sunshine_config_file
  ) noexcept {
    try {
      save_config_impl(response, std::move(request), path_for(sunshine_config_file));
    }
    catch (...) {
      write_unhandled_error(std::move(response));
    }
  }
}  // namespace ds5_config::api
