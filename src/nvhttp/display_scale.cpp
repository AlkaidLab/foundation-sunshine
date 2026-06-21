#include "display_scale.h"

#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <Simple-Web-Server/server_http.hpp>

#include "url_utils.h"
#include "src/logging.h"

#ifdef _WIN32
#include "src/display_device/display_device.h"
#include "src/platform/windows/display_device/windows_utils.h"
#endif

using json = nlohmann::json;

namespace nvhttp::display_scale {

  namespace {

    void
    log_request(const req_https_t &request) {
      BOOST_LOG(debug) << "Request - Protocol: HTTPS"
                       << ", IP: " << request->remote_endpoint().address().to_string()
                       << ", PORT: " << request->remote_endpoint().port()
                       << ", METHOD: " << request->method
                       << ", PATH: " << request->path;
    }

#ifdef _WIN32
    void
    add_fields(json &node, const display_device::w_utils::display_scale_info_t &scale_info) {
      node["is_primary"] = scale_info.is_primary;
      node["current_scale_percent"] = scale_info.current_scale_percent ? json(*scale_info.current_scale_percent) : json(nullptr);
      node["recommended_scale_percent"] = scale_info.recommended_scale_percent ? json(*scale_info.recommended_scale_percent) : json(nullptr);
      node["supported_scale_percents"] = scale_info.supported_scale_percents;
      node["scale_set_supported"] = scale_info.scale_set_supported;
    }
#endif

    void
    add_unsupported_fields(json &node) {
      node["is_primary"] = false;
      node["current_scale_percent"] = nullptr;
      node["recommended_scale_percent"] = nullptr;
      node["supported_scale_percents"] = json::array();
      node["scale_set_supported"] = false;
    }

#ifndef _WIN32
    void
    add_unsupported_scale_options_fields(json &node) {
      node["current_scale_percent"] = nullptr;
      node["recommended_scale_percent"] = nullptr;
      node["supported_scale_percents"] = json::array();
      node["scale_set_supported"] = false;
    }
#endif

#ifdef _WIN32
    json
    to_json(const display_device::w_utils::display_scale_info_t &scale_info) {
      json node {
        { "display_name", scale_info.display_name },
        { "device_id", scale_info.device_id },
        { "friendly_name", scale_info.friendly_name }
      };

      add_fields(node, scale_info);
      return node;
    }

    std::string
    error_code(display_device::w_utils::display_scale_error_e error) {
      switch (error) {
        case display_device::w_utils::display_scale_error_e::display_not_found:
          return "display_not_found";
        case display_device::w_utils::display_scale_error_e::unsupported_scale:
          return "unsupported_scale";
        case display_device::w_utils::display_scale_error_e::permission_denied:
          return "permission_denied";
        case display_device::w_utils::display_scale_error_e::apply_failed:
          return "apply_failed";
        case display_device::w_utils::display_scale_error_e::none:
        default:
          return "none";
      }
    }

    SimpleWeb::StatusCode
    http_status(display_device::w_utils::display_scale_error_e error) {
      switch (error) {
        case display_device::w_utils::display_scale_error_e::display_not_found:
          return SimpleWeb::StatusCode::client_error_not_found;
        case display_device::w_utils::display_scale_error_e::unsupported_scale:
          return SimpleWeb::StatusCode::client_error_bad_request;
        case display_device::w_utils::display_scale_error_e::permission_denied:
          return SimpleWeb::StatusCode::client_error_forbidden;
        case display_device::w_utils::display_scale_error_e::apply_failed:
          return SimpleWeb::StatusCode::server_error_internal_server_error;
        case display_device::w_utils::display_scale_error_e::none:
        default:
          return SimpleWeb::StatusCode::success_ok;
      }
    }
#endif

    SimpleWeb::CaseInsensitiveMultimap
    json_headers() {
      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "application/json");
      return headers;
    }

  }  // namespace

#ifdef _WIN32
  json
  build_windows_displays(const std::vector<std::string> &display_names) {
    std::unordered_map<std::string, std::pair<std::string, std::string>> display_info_map;
    try {
      for (const auto &device : display_device::enum_available_devices()) {
        const auto &device_id = device.first;
        if (std::string gdi_name = display_device::get_display_name(device_id); !gdi_name.empty()) {
          display_info_map[gdi_name] = { device_id, display_device::get_display_friendly_name(device_id) };
        }
      }
    }
    catch (const std::exception &e) {
      BOOST_LOG(warning) << "Failed to get display friendly names: " << e.what();
    }

    std::unordered_map<std::string, display_device::w_utils::display_scale_info_t> scale_info_by_display_name;
    std::unordered_map<std::string, display_device::w_utils::display_scale_info_t> scale_info_by_device_id;
    try {
      for (auto scale_info : display_device::w_utils::list_display_scale_info()) {
        if (!scale_info.display_name.empty()) {
          scale_info_by_display_name[scale_info.display_name] = scale_info;
        }
        if (!scale_info.device_id.empty()) {
          scale_info_by_device_id[scale_info.device_id] = std::move(scale_info);
        }
      }
    }
    catch (const std::exception &e) {
      BOOST_LOG(warning) << "Failed to get display scale info: " << e.what();
    }

    json displays_array = json::array();
    for (size_t i = 0; i < display_names.size(); ++i) {
      const auto &name = display_names[i];
      auto it = display_info_map.find(name);
      const bool found = it != display_info_map.end();
      const auto device_id = found ? it->second.first : std::string {};
      json display_node { { "index", static_cast<int>(i) },
        { "display_name", name },
        { "device_id", device_id },
        { "friendly_name", (found && !it->second.second.empty()) ? it->second.second : name } };

      auto scale_by_device = scale_info_by_device_id.find(device_id);
      auto scale_by_name = scale_info_by_display_name.find(name);
      if (scale_by_device != std::end(scale_info_by_device_id)) {
        add_fields(display_node, scale_by_device->second);
      }
      else if (scale_by_name != std::end(scale_info_by_display_name)) {
        add_fields(display_node, scale_by_name->second);
      }
      else {
        add_unsupported_fields(display_node);
      }

      displays_array.push_back(std::move(display_node));
    }

    return displays_array;
  }
#endif

  void
  get_options(resp_https_t response, req_https_t request) {
    log_request(request);

    json response_json;
    response_json["status_code"] = 200;
    response_json["status_message"] = "OK";

    auto headers = json_headers();

#ifdef _WIN32
    try {
      const auto args = request->parse_query_string();
      const auto display_name_it = args.find("display_name");
      const auto device_id_it = args.find("device_id");
      const std::string display_name = display_name_it != args.end() ? url_utils::decode(display_name_it->second) : std::string {};
      const std::string device_id = device_id_it != args.end() ? url_utils::decode(device_id_it->second) : std::string {};

      const auto scale_info = display_device::w_utils::get_display_scale_info(display_name, device_id);
      if (!scale_info) {
        response_json["status_code"] = 404;
        response_json["status_message"] = "Display not found";
        response_json["success"] = false;
        response_json["error_code"] = "display_not_found";
        response->write(SimpleWeb::StatusCode::client_error_not_found, response_json.dump(), headers);
        return;
      }

      response_json.update(to_json(*scale_info));
      response_json["success"] = true;
      response->write(SimpleWeb::StatusCode::success_ok, response_json.dump(), headers);
    }
    catch (const std::exception &e) {
      BOOST_LOG(error) << "Error getting display scale options: " << e.what();
      response_json["status_code"] = 500;
      response_json["status_message"] = "Internal server error: " + std::string(e.what());
      response_json["success"] = false;
      response_json["error_code"] = "apply_failed";
      response->write(SimpleWeb::StatusCode::server_error_internal_server_error, response_json.dump(), headers);
    }
#else
    response_json["status_code"] = 501;
    response_json["status_message"] = "Display scale is not supported on this platform";
    response_json["success"] = false;
    response_json["error_code"] = "unsupported_platform";
    add_unsupported_scale_options_fields(response_json);
    response->write(SimpleWeb::StatusCode::server_error_not_implemented, response_json.dump(), headers);
#endif
  }

  void
  set(resp_https_t response, req_https_t request) {
    log_request(request);

    json response_json;
    response_json["status_code"] = 200;
    response_json["status_message"] = "OK";

    auto headers = json_headers();

#ifdef _WIN32
    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      const auto body = json::parse(ss.str());

      if (!body.contains("scale_percent") || !body["scale_percent"].is_number_integer()) {
        response_json["status_code"] = 400;
        response_json["status_message"] = "Missing or invalid scale_percent";
        response_json["success"] = false;
        response_json["error_code"] = "invalid_request";
        response->write(SimpleWeb::StatusCode::client_error_bad_request, response_json.dump(), headers);
        return;
      }

      const std::string display_name = body.contains("display_name") && body["display_name"].is_string() ? body["display_name"].get<std::string>() : std::string {};
      const std::string device_id = body.contains("device_id") && body["device_id"].is_string() ? body["device_id"].get<std::string>() : std::string {};
      const int scale_percent = body["scale_percent"].get<int>();

      const auto set_result = display_device::w_utils::set_display_scale(display_name, device_id, scale_percent);
      response_json["success"] = set_result.success;
      response_json["display_name"] = set_result.display_name;
      response_json["device_id"] = set_result.device_id;
      response_json["previous_scale_percent"] = set_result.previous_scale_percent ? json(*set_result.previous_scale_percent) : json(nullptr);
      response_json["current_scale_percent"] = set_result.current_scale_percent ? json(*set_result.current_scale_percent) : json(nullptr);
      response_json["supported_scale_percents"] = set_result.supported_scale_percents;
      response_json["restart_required"] = set_result.restart_required;
      response_json["effective_immediately"] = set_result.effective_immediately;

      if (set_result.success) {
        response_json["status_message"] = set_result.message.empty() ? "OK" : set_result.message;
        response->write(SimpleWeb::StatusCode::success_ok, response_json.dump(), headers);
        return;
      }

      response_json["error_code"] = error_code(set_result.error);
      response_json["status_message"] = set_result.message.empty() ? "Failed to set display scale" : set_result.message;
      switch (set_result.error) {
        case display_device::w_utils::display_scale_error_e::display_not_found:
          response_json["status_code"] = 404;
          break;
        case display_device::w_utils::display_scale_error_e::permission_denied:
          response_json["status_code"] = 403;
          break;
        case display_device::w_utils::display_scale_error_e::unsupported_scale:
          response_json["status_code"] = 400;
          break;
        case display_device::w_utils::display_scale_error_e::apply_failed:
        case display_device::w_utils::display_scale_error_e::none:
        default:
          response_json["status_code"] = 500;
          break;
      }

      response->write(http_status(set_result.error), response_json.dump(), headers);
    }
    catch (const json::exception &e) {
      response_json["status_code"] = 400;
      response_json["status_message"] = "Invalid JSON body: " + std::string(e.what());
      response_json["success"] = false;
      response_json["error_code"] = "invalid_request";
      response->write(SimpleWeb::StatusCode::client_error_bad_request, response_json.dump(), headers);
    }
    catch (const std::exception &e) {
      BOOST_LOG(error) << "Error setting display scale: " << e.what();
      response_json["status_code"] = 500;
      response_json["status_message"] = "Internal server error: " + std::string(e.what());
      response_json["success"] = false;
      response_json["error_code"] = "apply_failed";
      response->write(SimpleWeb::StatusCode::server_error_internal_server_error, response_json.dump(), headers);
    }
#else
    response_json["status_code"] = 501;
    response_json["status_message"] = "Display scale is not supported on this platform";
    response_json["success"] = false;
    response_json["error_code"] = "unsupported_platform";
    response->write(SimpleWeb::StatusCode::server_error_not_implemented, response_json.dump(), headers);
#endif
  }

}  // namespace nvhttp::display_scale
