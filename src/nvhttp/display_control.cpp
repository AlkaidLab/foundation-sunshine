#include "display_control.h"

#include <chrono>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "src/http/server.h"
#include <nlohmann/json.hpp>

#include "display_scale.h"
#include "src/config.h"
#include "src/display_device/display_device.h"
#include "src/display_device/vdd_capability.h"
#include "src/logging.h"
#include "src/platform/common.h"

#ifdef _WIN32
  #include "src/platform/windows/display_device/windows_utils.h"
#endif

using json = nlohmann::json;

namespace nvhttp::display_control {

  namespace {

#ifdef _WIN32
    struct display_cache_t {
      std::mutex mutex;
      json displays;
      std::chrono::steady_clock::time_point updated_at {};
      bool valid { false };
    };

    display_cache_t display_cache;
#endif

    void
    log_request(const req_https_t &request) {
      BOOST_LOG(debug) << "Request - Protocol: HTTPS"
                       << ", IP: " << request->remote_endpoint().address().to_string()
                       << ", PORT: " << request->remote_endpoint().port()
                       << ", METHOD: " << request->method
                       << ", PATH: " << request->path;
    }

    web::CaseInsensitiveMultimap
    json_headers() {
      web::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "application/json");
      return headers;
    }

    web::StatusCode
    http_status_from_json(const json &response_json) {
      switch (response_json.value("status_code", 200)) {
        case 400:
          return web::StatusCode::bad_request;
        case 404:
          return web::StatusCode::not_found;
        case 500:
          return web::StatusCode::internal_server_error;
        case 501:
          return web::StatusCode::not_implemented;
        default:
          return web::StatusCode::ok;
      }
    }

  }  // namespace

  void
  get_displays(resp_https_t response, req_https_t request) {
    log_request(request);

    json response_json;
    response_json["status_code"] = 200;
    response_json["status_message"] = "OK";
    auto response_status = web::StatusCode::ok;

    try {
      json displays_array = json::array();

#ifndef _WIN32
      std::vector<std::string> display_names;
#endif

#ifdef _WIN32
      {
        std::lock_guard lock { display_cache.mutex };
        const auto now = std::chrono::steady_clock::now();
        if (display_cache.valid && now - display_cache.updated_at < std::chrono::seconds { 3 }) {
          displays_array = display_cache.displays;
        }
        else {
          displays_array = display_scale::build_windows_displays(platf::display_names(platf::mem_type_e::dxgi));
          display_cache.displays = displays_array;
          display_cache.updated_at = now;
          display_cache.valid = true;
        }
      }
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

#ifndef _WIN32
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

      response_json["count"] = static_cast<int>(displays_array.size());
      response_json["displays"] = std::move(displays_array);

      const auto vdd_state = display_device::vdd_capability::query_state();
      response_json["vdd"] = {
#ifdef _WIN32
        { "capability_version", display_device::vdd_capability::capability_version },
#else
        { "capability_version", 0 },
#endif
        { "state", display_device::vdd_capability::to_string(vdd_state) }
      };
    }
    catch (const std::exception &e) {
      BOOST_LOG(error) << "Error getting display list: " << e.what();
      response_json["status_code"] = 500;
      response_json["status_message"] = "Internal server error";
      response_json["displays"] = json::array();
      response_json["count"] = 0;
      response_status = web::StatusCode::internal_server_error;
    }

    response->write(response_status, response_json.dump(), json_headers());
  }

  void
  rotate(resp_https_t response, req_https_t request) {
    log_request(request);

    json response_json;
    response_json["status_code"] = 200;
    response_json["status_message"] = "OK";

    auto send_response = [&](web::StatusCode status_code) {
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
        send_response(web::StatusCode::bad_request);
        return;
      }

      int angle = std::stoi(angle_param->second);

      if (angle != 0 && angle != 90 && angle != 180 && angle != 270) {
        response_json["status_code"] = 400;
        response_json["status_message"] = "Invalid angle value. Must be 0, 90, 180, or 270";
        response_json["success"] = false;
        BOOST_LOG(warning) << "rotate_display: Invalid angle value: " << angle;
        send_response(web::StatusCode::bad_request);
        return;
      }

      auto display_name_param = args.find("display_name");
      std::string display_name = display_name_param != args.end() ? display_name_param->second : "";

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
