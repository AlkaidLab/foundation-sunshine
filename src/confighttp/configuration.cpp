/**
 * @file src/confighttp/configuration.cpp
 * @brief Web UI configuration handlers and routes.
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

#include "configuration.h"
#include "src/build_info.h"
#include "src/config.h"
#include "src/confighttp/apps.h"
#include "src/globals.h"
#include "src/confighttp/auth.h"
#include "src/confighttp/common.h"
#include "src/display_device/session.h"
#include "src/display_device/to_string.h"
#include "src/display_device/vdd_settings.h"
#include "src/doctor/doctor_os.h"
#include "src/file_handler.h"
#include "src/logging.h"
#include "src/nvenc/frame_budget.h"
#include "src/nvhttp.h"
#include "src/network.h"
#include "src/platform/common.h"
#include "src/settings_reset.h"
#include "src/sparxie/apps_file_lock.h"
#include "src/stream.h"
#include "src/telemetry/telemetry.h"
#include "src/utility.h"
#include "src/video.h"
#include <boost/property_tree/json_parser.hpp>
#include <chrono>
#include <sstream>

using namespace std::literals;
using json = nlohmann::json;

namespace confighttp::configuration {
  namespace pt = boost::property_tree;

  using namespace confighttp;

  namespace {
    void
    getListeners(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;
      json output {
        { "status", true },
        { "addresses", json::array() },
        { "active", {
          { "bind_address", config::sunshine.bind_address },
          { "address_family", config::sunshine.address_family },
          { "port", config::sunshine.port },
          { "upnp", static_cast<bool>(config::sunshine.flags[config::flag::UPNP]) },
          { "origin_web_ui_allowed", config::nvhttp.origin_web_ui_allowed },
        } },
      };
      try {
        const auto snapshot = config::get_config_snapshot();
        if (!snapshot) throw std::runtime_error("Unable to read saved listener settings");
        const auto value = [&](const std::string &key, const std::string &fallback) {
          const auto found = snapshot->find(key);
          return found == snapshot->end() ? fallback : found->second;
        };
        const auto upnp = value("upnp", "disabled");
        output["saved"] = {
          { "bind_address", value("bind_address", "") },
          { "address_family", value("address_family", "ipv4") },
          { "port", std::stoi(value("port", "47989")) },
          { "upnp", upnp == "enabled" || upnp == "true" || upnp == "1" },
          { "origin_web_ui_allowed", value("origin_web_ui_allowed", "lan") },
        };
        for (const auto &address : net::local_addresses()) output["addresses"].push_back({ { "name", address.name }, { "address", address.address } });
      }
      catch (const std::exception &error) {
        output["status"] = false;
        output["error"] = error.what();
      }
      send_response(response, output);
    }
    void
    getConfig(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;

      print_req(request);

      pt::ptree outputTree;
      auto g = util::fail_guard([&]() {
        std::ostringstream data;

        pt::write_json(data, outputTree);
        response->write(data.str());
      });

      auto devices { display_device::enum_available_devices() };
      pt::ptree devices_nodes;
      for (const auto &[device_id, data] : devices) {
        pt::ptree devices_node;
        devices_node.put("device_id"s, device_id);
        devices_node.put("data"s, to_string(data));
        devices_nodes.push_back(std::make_pair(""s, devices_node));
      }

      auto adapters { platf::adapter_names() };
      pt::ptree adapters_nodes;
      for (const auto &adapter_name : adapters) {
        pt::ptree adapters_node;
        adapters_node.put("name"s, adapter_name);
        adapters_nodes.push_back(std::make_pair(""s, adapters_node));
      }

      outputTree.add_child("display_devices", devices_nodes);
      outputTree.add_child("adapters", adapters_nodes);
      outputTree.put("status", "true");
      outputTree.put("platform", SUNSHINE_PLATFORM);
      // The Windows build the host runs on (RtlGetVersion), for the first-run guide: the base virtual display has no HDR on Windows 10 (build 19045 at most). Left out where it is unknown.
      if (const auto windows_build = doctor::os::windows_build()) outputTree.put("windows_build", *windows_build);
      outputTree.put("version", build_info::version());

      auto vars = config::parse_config(file_handler::read_file(config::sunshine.config_file.c_str()));
      for (auto &[name, value] : vars) {
        // widget_token 是本地端点凭证,不回显明文;只暴露是否已配置
        if (name == "widget_token" || name == "mat_live_pipe" || name == "mat_live_producer") {
          continue;
        }
        outputTree.put(std::move(name), std::move(value));
      }
      outputTree.put("widget_token_configured", !config::sunshine.widget_token.empty());

#ifdef _WIN32
      const auto vsync = nvprefs_instance.status();
      outputTree.put("nvidia_vsync.supported", vsync.supported);
      outputTree.put("nvidia_vsync.detection_known", vsync.detection_known);
      outputTree.put("nvidia_vsync.state", vsync.state);
      outputTree.put("nvidia_vsync.version", 1);
#endif
      outputTree.put("active_encoder", video::active_encoder_name());
      // what the encoder probe resolved the two codec settings to (0 = not probed yet, 1 = not offered, 2 = offered, 3 = offered with 10-bit)
      outputTree.put("active_hevc_mode", video::active_hevc_mode);
      outputTree.put("active_av1_mode", video::active_av1_mode);
      if (auto frame_budget = nvenc::get_frame_budget_report()) {
        pt::ptree budget_node;
        budget_node.put("clamped", frame_budget->clamped);
        budget_node.put("configured_preset", frame_budget->configured_preset);
        budget_node.put("effective_preset", frame_budget->effective_preset);
        budget_node.put("width", frame_budget->width);
        budget_node.put("height", frame_budget->height);
        budget_node.put("fps", frame_budget->fps);
        budget_node.put("budget_ms", frame_budget->budget_ms);
        budget_node.put("estimated_ms", frame_budget->estimated_ms);
        budget_node.put("configured_estimated_ms", frame_budget->configured_estimated_ms);
        budget_node.put("num_engines", frame_budget->num_engines);
        outputTree.add_child("active_nvenc_frame_budget", budget_node);
      }
      // Configuration capability only; never expose the paired-client tunnel token here.
      outputTree.put("usb_forwarding_config_version", "1");
      outputTree.put("pair_name", nvhttp::get_pair_name());
    }

    void
    getLocale(resp_https_t response, req_https_t request) {
      // we need to return the locale whether authenticated or not

      print_req(request);

      pt::ptree outputTree;
      auto g = util::fail_guard([&]() {
        std::ostringstream data;

        pt::write_json(data, outputTree);
        response->write(data.str());
      });

      outputTree.put("status", "true");
      outputTree.put("locale", config::sunshine.locale);
    }

    void
    getGamepadConfig(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;

      pt::ptree outputTree;
      auto response_guard = util::fail_guard([&]() {
        std::ostringstream data;
        pt::write_json(data, outputTree);
        response->write(data.str());
      });

      const auto config_snapshot = config::get_config_snapshot();
      if (!config_snapshot) {
        outputTree.put("status", "false");
        outputTree.put("error", "failed to read controller configuration");
        return;
      }

      const auto get_value = [&](std::string_view key, std::string_view fallback) {
        const auto entry = config_snapshot->find(std::string { key });
        return entry == config_snapshot->end() ? std::string { fallback } : entry->second;
      };
      outputTree.put("status", "true");
      outputTree.put("gamepad", get_value("gamepad", "auto"));
      outputTree.put("motion_as_ds4", get_value("motion_as_ds4", "true"));
      outputTree.put("touchpad_as_ds4", get_value("touchpad_as_ds4", "true"));
      outputTree.put("ds4_back_as_touchpad_click", get_value("ds4_back_as_touchpad_click", "true"));
      outputTree.put("enable_dsu_server", get_value("enable_dsu_server", "false"));
      outputTree.put("dsu_server_port", get_value("dsu_server_port", "26760"));
    }

    void
    saveGamepadConfig(resp_https_t response, req_https_t request) {
      if (!check_content_type(response, request, "application/json")) return;
      if (!authenticate(response, request)) return;

      pt::ptree outputTree;
      auto response_guard = util::fail_guard([&]() {
        std::ostringstream data;
        pt::write_json(data, outputTree);
        response->write(data.str());
      });

      try {
        pt::ptree inputTree;
        std::stringstream body;
        body << request->content.rdbuf();
        pt::read_json(body, inputTree);
        if (inputTree.empty() || inputTree.size() > 6) {
          throw std::invalid_argument("controller configuration patch must contain 1 to 6 fields");
        }

        const std::set<std::string> boolean_fields {
          "ds4_back_as_touchpad_click",
          "enable_dsu_server",
          "motion_as_ds4",
          "touchpad_as_ds4",
        };
        std::map<std::string, std::string> updates;
        for (const auto &[key, node] : inputTree) {
          if (!node.empty()) {
            throw std::invalid_argument("controller configuration fields must be scalar values");
          }
          const auto value = node.get_value<std::string>();
          if (key == "gamepad") {
            if (value != "auto"sv && value != "x360"sv && value != "ds4"sv && value != "ds5"sv) {
              throw std::invalid_argument("invalid gamepad mode");
            }
          }
          else if (boolean_fields.contains(key)) {
            if (value != "true"sv && value != "false"sv) {
              throw std::invalid_argument("controller boolean fields must be true or false");
            }
          }
          else if (key == "dsu_server_port") {
            std::size_t parsed = 0;
            const auto port = std::stoi(value, &parsed);
            if (parsed != value.size() || port < 1024 || port > 65535) {
              throw std::invalid_argument("DSU port must be between 1024 and 65535");
            }
          }
          else {
            throw std::invalid_argument("unsupported controller configuration field");
          }
          if (!updates.emplace(key, value).second) {
            throw std::invalid_argument("duplicate controller configuration field");
          }
        }

        if (!config::update_config(updates)) {
          outputTree.put("status", "false");
          outputTree.put("error", "failed to persist controller configuration");
          return;
        }
        outputTree.put("status", "true");
      }
      catch (const std::exception &e) {
        BOOST_LOG(warning) << "SaveGamepadConfig: "sv << e.what();
        outputTree.put("status", "false");
        outputTree.put("error", "invalid controller configuration patch");
      }
    }

    void
    saveConfig(resp_https_t response, req_https_t request) {
      if (!check_content_type(response, request, "application/json")) return;
      if (!authenticate(response, request)) return;

      print_req(request);

      std::stringstream ss;
      ss << request->content.rdbuf();
      pt::ptree outputTree;
      auto g = util::fail_guard([&]() {
        std::ostringstream data;

        pt::write_json(data, outputTree);
        response->write(data.str());
      });

      pt::ptree inputTree;

      try {
        pt::read_json(ss, inputTree);
        if (const auto mat = inputTree.get_optional<std::string>("mat_live"); mat && *mat != "true" && *mat != "false" && *mat != "enabled" && *mat != "disabled") {
          throw std::invalid_argument("mat_live must be enabled or disabled");
        }
        net::validate_bind_address(inputTree.get<std::string>("bind_address", ""), inputTree.get<std::string>("address_family", "ipv4"));
        const auto port_value = inputTree.get<std::string>("port", "47989");
        std::size_t consumed = 0;
        const auto port = std::stoi(port_value, &consumed);
        if (consumed != port_value.size() || port < 1029 || port > 65514) throw std::invalid_argument("port must be between 1029 and 65514");
        if (const auto upnp = inputTree.get_optional<std::string>("upnp"); upnp && *upnp != "enabled" && *upnp != "disabled" && *upnp != "true" && *upnp != "false") {
          throw std::invalid_argument("invalid upnp value");
        }
        std::string resArray = inputTree.get<std::string>("resolutions", "[]");
        std::string fpsArray = inputTree.get<std::string>("fps", "[]");
        std::string gpu_name = inputTree.get<std::string>("adapter_name", "");

        // Validate config field lengths to prevent abuse
        auto sunshine_name = inputTree.get<std::string>("sunshine_name", "");
        if (sunshine_name.size() > 256) {
          outputTree.put("status", "false");
          outputTree.put("error", "sunshine_name too long (max 256)");
          return;
        }
        if (gpu_name.size() > 256) {
          outputTree.put("status", "false");
          outputTree.put("error", "adapter_name too long (max 256)");
          return;
        }

        display_device::vdd_settings::save(resArray, fpsArray, gpu_name);

        // 将 inputTree 转换为 std::map（保证有序）
        std::map<std::string, std::string> fullConfig;
        for (const auto &kv : inputTree) {
          if (kv.first == "usb_forwarding_config_version" || kv.first == "mat_live_pipe" || kv.first == "mat_live_producer") continue;
          std::string value = inputTree.get<std::string>(kv.first);
          fullConfig[kv.first] = value;
        }

        // 更新配置
        std::string previous_locale;
        // the settings that are reported when they change (an allow-list of keys, values as enums or buckets) are compared with what was saved before
        const auto saved_before = config::get_config_snapshot();
        if (!config::update_full_config(fullConfig, &previous_locale)) {
          outputTree.put("status", "false");
          outputTree.put("error", "failed to persist configuration");
          return;
        }
        // the two telemetry choices take effect at once: what the off kind had waiting is dropped
        telemetry::consent_changed();
        if (saved_before) telemetry::settings_changed(*saved_before, fullConfig);
        // the language of the host changed: the built-in applications show their names in it (a name the person changed stays)
        const auto locale_now = fullConfig.find("locale");
        if (locale_now != fullConfig.end() && !locale_now->second.empty() && locale_now->second != previous_locale) {
          confighttp::apps::retitle_defaults(locale_now->second);
        }

        // The bitrate ceiling takes effect now: update_full_config set it in this process, the running sessions take it before the answer goes out (so the tray, which waits for it, can say it is applied).
        {
          const auto changed = stream::session::apply_host_bitrate_cap();
          outputTree.put("max_bitrate_in_force", config::host_max_bitrate_kbps());
          outputTree.put("sessions_updated", static_cast<int>(changed.size()));
        }
      }
      catch (std::exception &e) {
        BOOST_LOG(warning) << "SaveConfig: "sv << e.what();
        outputTree.put("status", "false");
        outputTree.put("error", e.what());
        return;
      }

      outputTree.put("status", "true");
    }

    // ---- 恢复默认设置: copy sunshine.conf, write the defaults, restart (src/settings_reset.h). Paired devices live in the state file and are not touched.
    settings_reset::gate_t &
    reset_gate() {
      static settings_reset::gate_t gate;
      return gate;
    }

    settings_reset::paths_t
    reset_paths() {
      return { file_handler::path_from_utf8(config::sunshine.config_file), file_handler::path_from_utf8(config::stream.file_apps), file_handler::path_from_utf8(SUNSHINE_ASSETS_DIR "/apps.json") };
    }

    settings_reset::parse_t
    reset_parser() {
      return [](std::string_view text) {
        const auto parsed = config::parse_config(text);
        return std::map<std::string, std::string> { parsed.begin(), parsed.end() };
      };
    }

    settings_reset::now_t
    reset_clock() {
      return [] { return static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()); };
    }

    web::StatusCode
    reset_status_of(settings_reset::error_e error) {
      switch (error) {
        case settings_reset::error_e::none:
          return web::StatusCode::ok;
        case settings_reset::error_e::busy:
          return web::StatusCode::conflict;
        case settings_reset::error_e::no_backup:
          return web::StatusCode::not_found;
        default:
          return web::StatusCode::internal_server_error;
      }
    }

    void
    write_reset_error(resp_https_t response, settings_reset::error_e error) {
      auto headers = json_headers();
      response->write(reset_status_of(error), nlohmann::json { { "status", false }, { "code", std::string { settings_reset::error_code(error) } }, { "error", std::string { settings_reset::error_code(error) } } }.dump(), headers);
    }

    /// The copies that exist (newest first), for the page that offers the restore.
    void
    getConfigReset(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;
      json output { { "status", true }, { "limit", settings_reset::kept_copies }, { "backups", json::array() } };
      for (const auto &backup : settings_reset::list_backups(reset_paths())) {
        output["backups"].push_back({ { "stamp", backup.stamp }, { "created_ms", backup.created_ms }, { "has_apps", backup.has_apps } });
      }
      send_response(response, output);
    }

    /// Both POST routes answer first and restart after the answer has gone out (the page waits for the host to come back).
    void
    answer_and_restart(resp_https_t response, const json &body) {
      web::CaseInsensitiveMultimap headers = json_headers();
      response->write(body.dump(), headers);
      response->send([](const auto &error) {
        if (error) BOOST_LOG(debug) << "Settings reset: the answer did not reach the client: "sv << error.message();
        platf::restart("settings");
      });
    }

    void
    resetConfig(resp_https_t response, req_https_t request) {
      if (!check_content_type(response, request, "application/json")) return;
      if (!authenticate(response, request)) return;
      print_req(request);

      json body;
      if (!read_json_body(response, request, body)) return;
      const auto clear_apps = body.is_object() ? body.find("clear_apps") : body.end();
      if (!body.is_object() || (clear_apps != body.end() && !clear_apps->is_boolean())) {
        write_json(std::move(response), web::StatusCode::bad_request, { { "status", false }, { "code", "bad_request" }, { "error", "clear_apps must be true or false" } });
        return;
      }
      const bool with_apps = clear_apps != body.end() && clear_apps->get<bool>();

      auto &gate = reset_gate();
      if (!gate.try_enter()) {
        write_reset_error(std::move(response), settings_reset::error_e::busy);
        return;
      }
      settings_reset::result_t result;
      // an exception on the way (a path that cannot be converted, no memory) opens the gate again: a failed call must not keep every later one out
      auto open_gate = util::fail_guard([&gate] { gate.leave(false); });
      {
        // the application list is written by other routes under its own lock: the order is that lock, then the settings file's
        std::lock_guard apps_lock(::sparxie::apps_file_mutex);
        config::with_config_file_locked([&] { result = settings_reset::reset_to_defaults(reset_paths(), with_apps, reset_parser(), reset_clock()); });
      }
      open_gate.disable();
      gate.leave(static_cast<bool>(result));
      if (!result) {
        BOOST_LOG(warning) << "Settings reset failed: "sv << settings_reset::error_code(result.error);
        write_reset_error(std::move(response), result.error);
        return;
      }
      BOOST_LOG(info) << "Settings reset to the defaults (application list "sv << (result.apps_changed ? "replaced"sv : "kept"sv) << "), restarting"sv;
      answer_and_restart(std::move(response), { { "status", true }, { "backup", result.backup }, { "apps_cleared", result.apps_changed }, { "restarting", true } });
    }

    void
    restoreConfigReset(resp_https_t response, req_https_t request) {
      if (!check_content_type(response, request, "application/json")) return;
      if (!authenticate(response, request)) return;
      print_req(request);

      auto &gate = reset_gate();
      if (!gate.try_enter()) {
        write_reset_error(std::move(response), settings_reset::error_e::busy);
        return;
      }
      settings_reset::result_t result;
      auto open_gate = util::fail_guard([&gate] { gate.leave(false); });
      {
        std::lock_guard apps_lock(::sparxie::apps_file_mutex);
        config::with_config_file_locked([&] { result = settings_reset::restore_latest(reset_paths(), reset_parser(), reset_clock()); });
      }
      open_gate.disable();
      gate.leave(static_cast<bool>(result));
      if (!result) {
        BOOST_LOG(warning) << "Settings restore failed: "sv << settings_reset::error_code(result.error);
        write_reset_error(std::move(response), result.error);
        return;
      }
      BOOST_LOG(info) << "Settings restored from the copy of "sv << result.restored << ", restarting"sv;
      answer_and_restart(std::move(response), { { "status", true }, { "restored", result.restored }, { "backup", result.backup }, { "apps_restored", result.apps_changed }, { "restarting", true } });
    }

  }  // namespace

  void
  register_routes(https_server_t &server) {
    server.resource["^/api/config$"]["GET"] = getConfig;
    server.resource["^/api/config$"]["POST"] = saveConfig;
    server.resource["^/api/config/reset$"]["GET"] = getConfigReset;
    server.resource["^/api/config/reset$"]["POST"] = resetConfig;
    server.resource["^/api/config/reset/restore$"]["POST"] = restoreConfigReset;
    server.resource["^/api/gamepad/config$"]["GET"] = getGamepadConfig;
    server.resource["^/api/gamepad/config$"]["POST"] = saveGamepadConfig;
    server.resource["^/api/configLocale$"]["GET"] = getLocale;
    server.resource["^/api/network/listeners$"]["GET"] = getListeners;
  }

}  // namespace confighttp::configuration
