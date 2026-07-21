#include "display_control.h"

#include <string>
#include <utility>
#include <vector>

#include <Simple-Web-Server/server_http.hpp>
#include <nlohmann/json.hpp>

#include "display_scale.h"
#include "url_utils.h"
#include "src/config.h"
#include "src/display_device/display_device.h"
#include "src/logging.h"
#include "src/platform/common.h"

#ifdef _WIN32
#include "src/platform/windows/display_device/windows_utils.h"
#endif

using json = nlohmann::json;

namespace nvhttp::display_control {

  namespace {

    void
    log_request(const req_https_t &request) {
      BOOST_LOG(debug) << "Request - Protocol: HTTPS"
                       << ", IP: " << request->remote_endpoint().address().to_string()
                       << ", PORT: " << request->remote_endpoint().port()
                       << ", METHOD: " << request->method
                       << ", PATH: " << request->path;
    }

    SimpleWeb::CaseInsensitiveMultimap
    json_headers() {
      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "application/json");
      return headers;
    }

    SimpleWeb::StatusCode
    http_status_from_json(const json &response_json) {
      switch (response_json.value("status_code", 200)) {
        case 400:
          return SimpleWeb::StatusCode::client_error_bad_request;
        case 404:
          return SimpleWeb::StatusCode::client_error_not_found;
        case 500:
          return SimpleWeb::StatusCode::server_error_internal_server_error;
        case 501:
          return SimpleWeb::StatusCode::server_error_not_implemented;
        default:
          return SimpleWeb::StatusCode::success_ok;
      }
    }

  }  // namespace

  void
  get_displays(resp_https_t response, req_https_t request) {
    log_request(request);

    json response_json;
    response_json["status_code"] = 200;
    response_json["status_message"] = "OK";
    auto response_status = SimpleWeb::StatusCode::success_ok;

    try {
      std::vector<std::string> display_names;

#ifdef _WIN32
      display_names = platf::display_names(platf::mem_type_e::dxgi);
#elif defined(__linux__)
      for (auto mem_type : { platf::mem_type_e::vaapi, platf::mem_type_e::cuda, platf::mem_type_e::system }) {
        display_names = platf::display_names(mem_type);
        if (!display_names.empty()) break;
      }
#elif defined(__APPLE__)
      display_names = platf::display_names(platf::mem_type_e::videotoolbox);
#else
      display_names = platf::display_names(platf::mem_type_e::system);
#endif

      json displays_array = json::array();

#ifdef _WIN32
      displays_array = display_scale::build_windows_displays(display_names);
#else
      for (size_t i = 0; i < display_names.size(); ++i) {
        const auto &name = display_names[i];
        displays_array.push_back({ { "index", static_cast<int>(i) },
          { "display_name", name },
          { "device_id", name },
          { "friendly_name", name },
          { "is_primary", false },
          { "current_scale_percent", nullptr },
          { "recommended_scale_percent", nullptr },
          { "supported_scale_percents", json::array() },
          { "scale_set_supported", false } });
      }
#endif

      response_json["displays"] = std::move(displays_array);
      response_json["count"] = static_cast<int>(display_names.size());
    }
    catch (const std::exception &e) {
      BOOST_LOG(error) << "Error getting display list: " << e.what();
      response_json["status_code"] = 500;
      response_json["status_message"] = "Internal server error";
      response_json["displays"] = json::array();
      response_json["count"] = 0;
      response_status = SimpleWeb::StatusCode::server_error_internal_server_error;
    }

    response->write(response_status, response_json.dump(), json_headers());
  }

  void
  rotate(resp_https_t response, req_https_t request) {
    log_request(request);

    json response_json;
    response_json["status_code"] = 200;
    response_json["status_message"] = "OK";

    auto send_response = [&](SimpleWeb::StatusCode status_code) {
      response->write(status_code, response_json.dump(), json_headers());
      response->close_connection_after_response = true;
    };

    try {
      auto args = request->parse_query_string();
      auto angle_param = args.find("angle");

      if (angle_param == args.end()) {
        response_json["status_code"] = 400;
        response_json["status_message"] = "Missing angle parameter";
        response_json["success"] = false;
        BOOST_LOG(warning) << "rotate_display: Missing angle parameter";
        send_response(SimpleWeb::StatusCode::client_error_bad_request);
        return;
      }

      int angle = std::stoi(angle_param->second);

      if (angle != 0 && angle != 90 && angle != 180 && angle != 270) {
        response_json["status_code"] = 400;
        response_json["status_message"] = "Invalid angle value. Must be 0, 90, 180, or 270";
        response_json["success"] = false;
        BOOST_LOG(warning) << "rotate_display: Invalid angle value: " << angle;
        send_response(SimpleWeb::StatusCode::client_error_bad_request);
        return;
      }

      auto display_name_param = args.find("display_name");
      std::string display_name = display_name_param != args.end() ? display_name_param->second : "";
      if (!display_name.empty()) {
        display_name = url_utils::decode(std::move(display_name));
      }

      if (display_name.empty() && !config::video.output_name.empty()) {
        display_name = display_device::get_display_name(config::video.output_name);
        if (display_name.empty()) {
          display_name = config::video.output_name;
        }
        BOOST_LOG(debug) << "rotate_display: Using current capture display: " << display_name << " (from config: " << config::video.output_name << ")";
      }

      BOOST_LOG(info) << "rotate_display: Requested angle=" << angle << ", display_name=" << (display_name.empty() ? "(primary)" : display_name);

#ifdef _WIN32
      bool success = display_device::w_utils::rotate_display(angle, display_name);
      if (success) {
        response_json["success"] = true;
        response_json["angle"] = angle;
        response_json["message"] = "Display rotation changed successfully";
        BOOST_LOG(info) << "rotate_display: Display rotation changed to " << angle << " degrees";
      }
      else {
        response_json["status_code"] = 500;
        response_json["status_message"] = "Failed to change display rotation";
        response_json["success"] = false;
        BOOST_LOG(error) << "rotate_display: Failed to change display rotation to " << angle << " degrees";
      }
#else
      response_json["status_code"] = 501;
      response_json["status_message"] = "Display rotation is not supported on this platform";
      response_json["success"] = false;
      BOOST_LOG(warning) << "rotate_display: Display rotation is not supported on this platform";
#endif
    }
    catch (const std::exception &e) {
      BOOST_LOG(error) << "Error rotating display: " << e.what();
      response_json["status_code"] = 500;
      response_json["status_message"] = "Internal server error: " + std::string(e.what());
      response_json["success"] = false;
    }

    send_response(http_status_from_json(response_json));
  }

}  // namespace nvhttp::display_control
