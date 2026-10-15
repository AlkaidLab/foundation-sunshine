/**
 * @file src/confighttp/configuration.cpp
 * @brief Web UI configuration handlers and routes.
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

#include "configuration.h"
#include "src/config.h"
#include "src/globals.h"
#include "src/confighttp/auth.h"
#include "src/confighttp/common.h"
#include "src/display_device/session.h"
#include "src/display_device/to_string.h"
#include "src/display_device/vdd_settings.h"
#include "src/file_handler.h"
#include "src/logging.h"
#include "src/nvenc/frame_budget.h"
#include "src/nvhttp.h"
#include "src/platform/common.h"
#include "src/utility.h"
#include "src/video.h"
#include "version.h"
#include <boost/property_tree/json_parser.hpp>
#include <sstream>

using namespace std::literals;
using json = nlohmann::json;

namespace confighttp::configuration {
  namespace pt = boost::property_tree;

  using namespace confighttp;

  namespace {
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
      outputTree.put("version", PROJECT_VER);

      auto vars = config::parse_config(file_handler::read_file(config::sunshine.config_file.c_str()));
      for (auto &[name, value] : vars) {
        // widget_token 是本地端点凭证,不回显明文;只暴露是否已配置
        if (name == "widget_token") {
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
          if (kv.first == "usb_forwarding_config_version") continue;
          std::string value = inputTree.get<std::string>(kv.first);
          fullConfig[kv.first] = value;
        }

        // 更新配置
        if (!config::update_full_config(fullConfig)) {
          outputTree.put("status", "false");
          outputTree.put("error", "failed to persist configuration");
          return;
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

  }  // namespace

  void
  register_routes(https_server_t &server) {
    server.resource["^/api/config$"]["GET"] = getConfig;
    server.resource["^/api/config$"]["POST"] = saveConfig;
    server.resource["^/api/gamepad/config$"]["GET"] = getGamepadConfig;
    server.resource["^/api/gamepad/config$"]["POST"] = saveGamepadConfig;
    server.resource["^/api/configLocale$"]["GET"] = getLocale;
  }

}  // namespace confighttp::configuration
