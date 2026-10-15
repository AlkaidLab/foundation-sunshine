/**
 * @file src/confighttp/runtime.cpp
 * @brief Web UI runtime handlers and routes.
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

#include "runtime.h"
#include "src/config.h"
#include "src/confighttp/auth.h"
#include "src/confighttp/common.h"
#include "src/display_device/color_profile.h"
#include "src/display_device/display_device.h"
#include "src/globals.h"
#include "src/image_enhancement/config.h"
#include "src/logging.h"
#include "src/rtsp.h"
#include "src/stream.h"
#include "src/streaming/perf_recorder.h"
#include "src/video.h"
#include <boost/algorithm/string.hpp>
#include <cmath>
#include <set>
#include <sstream>
#ifdef _WIN32
  #include "src/platform/windows/display_device/color_profile.h"
#endif

using namespace std::literals;
using json = nlohmann::json;

namespace confighttp::runtime {

  using namespace confighttp;

  namespace {
    void
    getRuntimeSessions(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;

      print_req(request);

      if (!require_localhost(response, request, "getting runtime sessions")) {
        return;
      }

      try {
        // 获取所有活动会话信息
        auto sessions_info = stream::session::get_all_sessions_info();

        json response_json;
        response_json["success"] = true;
        response_json["status_code"] = 200;
        response_json["status_message"] = "Success";
        response_json["total_sessions"] = sessions_info.size();

        json sessions_array = json::array();

        for (const auto &session_info : sessions_info) {
          json session_obj;
          session_obj["client_name"] = session_info.client_name;
          session_obj["client_uuid"] = session_info.client_uuid;
          session_obj["client_address"] = session_info.client_address;
          session_obj["state"] = session_info.state;
          session_obj["session_id"] = session_info.session_id;
          session_obj["width"] = session_info.width;
          session_obj["height"] = session_info.height;
          session_obj["fps"] = session_info.fps;
          session_obj["bitrate"] = session_info.bitrate;
          session_obj["host_audio"] = session_info.host_audio;
          session_obj["enable_hdr"] = session_info.enable_hdr;
          session_obj["enable_mic"] = session_info.enable_mic;
          session_obj["use_vdd"] = session_info.use_vdd;
          session_obj["hdr_brightness_reported"] = session_info.hdr_brightness_reported;
          session_obj["hdr_brightness_source"] = session_info.hdr_brightness_source;
          session_obj["hdr_max_nits"] = session_info.hdr_max_nits;
          session_obj["hdr_min_nits"] = session_info.hdr_min_nits;
          session_obj["hdr_max_full_frame_nits"] = session_info.hdr_max_full_frame_nits;
          session_obj["app_name"] = session_info.app_name;
          session_obj["app_id"] = session_info.app_id;

          sessions_array.push_back(session_obj);
        }

        response_json["sessions"] = sessions_array;

        BOOST_LOG(debug) << "Config API: Runtime sessions info requested, returned " << sessions_info.size() << " sessions";

        response->write(response_json.dump());
        response->close_connection_after_response = true;
      }
      catch (const std::exception &e) {
        BOOST_LOG(error) << "getRuntimeSessions: " << e.what();
        write_runtime_error(response, web::StatusCode::internal_server_error, 500, std::string(e.what()));
      }
      catch (...) {
        BOOST_LOG(error) << "getRuntimeSessions: Unknown exception";
        write_runtime_error(response, web::StatusCode::internal_server_error, 500, "Unknown error");
      }
    }

    void
    getRuntimeHdrStatus(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;

      print_req(request);

      if (!require_localhost(response, request, "getting runtime HDR status")) {
        return;
      }

      try {
        const auto statuses = video::get_hdr_pipeline_statuses();
        const auto enhancement_status = image_enhancement::manager().status();
        json response_json {
          { "success", true },
          { "status_code", 200 },
          { "status_message", "Success" },
#ifdef _WIN32
          { "available", true },
#else
          { "available", false },
#endif
          { "configured_analysis_mode", config::video.hdr_luminance_analysis },
          { "configured_conversion_mode", config::video.capture_compute_shader },
          { "configured_hdr_backend", enhancement_status.value("selected_backend", std::string {}) },
          { "configured_nr_backend", enhancement_status.value("selected_nr_backend", std::string {}) },
          { "pipelines", json::array() },
        };

        for (const auto &status : statuses) {
          response_json["pipelines"].push_back({
            { "id", status.id },
            { "hdr_mode", status.hdr_mode },
            { "analysis_mode", status.analysis_mode },
            { "analysis_active", status.analysis_active },
            { "scene_metadata_active", status.scene_metadata_active },
            { "metadata_formats", status.metadata_formats },
            { "conversion_path", status.conversion_path },
            { "conversion_fallback_reason", status.conversion_fallback_reason },
            { "analysis_failure_reason", status.analysis_failure_reason },
            { "synthetic_hdr_backend", status.synthetic_hdr_backend },
            { "synthetic_hdr_state", status.synthetic_hdr_state },
            { "synthetic_hdr_failure_reason", status.synthetic_hdr_failure_reason },
            { "nr_backend", status.nr_backend },
            { "nr_state", status.nr_state },
            { "nr_failure_reason", status.nr_failure_reason },
          });
        }

        response->write(response_json.dump());
        response->close_connection_after_response = true;
      }
      catch (const std::exception &e) {
        BOOST_LOG(error) << "getRuntimeHdrStatus: " << e.what();
        write_runtime_error(response, web::StatusCode::internal_server_error, 500, std::string(e.what()));
      }
      catch (...) {
        BOOST_LOG(error) << "getRuntimeHdrStatus: Unknown exception";
        write_runtime_error(response, web::StatusCode::internal_server_error, 500, "Unknown error");
      }
    }

    void
    getRuntimeHdrCalibration(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;
      print_req(request);
      if (!require_localhost(response, request, "getting HDR calibration status")) return;

      try {
        json output {
          { "success", true },
          { "supported", false },
          { "vddActive", false },
          { "hdrEnabled", false },
          { "calibrated", false },
          { "profileName", nullptr },
          { "maxNits", nullptr },
          { "minNits", nullptr },
          { "maxFullFrameNits", nullptr },
          { "state", "unsupported" },
          { "sharedVdd", config::video.vdd_reuse },
          { "activeClients", json::array() },
        };

        for (const auto &session : stream::session::get_all_sessions_info()) {
          if (session.state == "RUNNING" && session.enable_hdr && session.use_vdd) {
            output["activeClients"].push_back({
              { "uuid", session.client_uuid },
              { "name", session.client_name },
            });
          }
        }

#ifdef _WIN32
        output["supported"] = true;
        const auto vdd_id = display_device::find_device_by_friendlyname(ZAKO_NAME);
        if (!vdd_id.empty()) {
          output["vddActive"] = true;
          const auto hdr_states = display_device::get_current_hdr_states({ vdd_id });
          const auto hdr_state = hdr_states.find(vdd_id);
          const bool hdr_enabled = hdr_state != hdr_states.end() &&
                                   hdr_state->second == display_device::hdr_state_e::enabled;
          output["hdrEnabled"] = hdr_enabled;
          output["state"] = hdr_enabled ? "ready" : "waiting_hdr";

          if (const auto calibration = display_device::win_color_profile::current_hdr_calibration(vdd_id)) {
            output["calibrated"] = true;
            output["profileName"] = calibration->profile_name;
            output["maxNits"] = calibration->max_nits;
            output["minNits"] = calibration->min_nits;
            output["maxFullFrameNits"] = calibration->max_full_frame_nits;
            output["state"] = "calibrated";
          }
        }
        else {
          output["state"] = "waiting_vdd";
        }
#endif

        send_response(response, output);
      }
      catch (const std::exception &e) {
        BOOST_LOG(error) << "getRuntimeHdrCalibration: " << e.what();
        write_runtime_error(response, web::StatusCode::internal_server_error, 500, e.what());
      }
      catch (...) {
        BOOST_LOG(error) << "getRuntimeHdrCalibration: Unknown exception";
        write_runtime_error(response, web::StatusCode::internal_server_error, 500, "Unknown error");
      }
    }

    void
    stopRuntimeSessions(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;

      print_req(request);

      if (!require_localhost(response, request, "stopping runtime sessions")) {
        return;
      }

      try {
        // 终止当前全部串流会话(所有客户端),语义与 Game Bar widget 的 stop_all_sessions 一致
        rtsp_stream::terminate_sessions_async(
          stream::session::stop_reason_e::host_terminate,
          boost::function<void()>([] {}));
        BOOST_LOG(info) << "Config API: session termination requested from local panel"sv;

        response->write(json {
                          { "success", true },
                          { "status_message", "session termination requested" },
                        }
                          .dump(),
          { { "Content-Type", "application/json" } });
        response->close_connection_after_response = true;
      }
      catch (const std::exception &e) {
        BOOST_LOG(error) << "stopRuntimeSessions: " << e.what();
        write_runtime_error(response, web::StatusCode::internal_server_error, 500, e.what());
      }
      catch (...) {
        BOOST_LOG(error) << "stopRuntimeSessions: Unknown exception";
        write_runtime_error(response, web::StatusCode::internal_server_error, 500, "Unknown error");
      }
    }

    void
    getPerfCurrent(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;

      print_req(request);

      if (!require_localhost(response, request, "getting performance snapshot")) {
        return;
      }

      try {
        response->write(perf::current_snapshot_json().dump());
        response->close_connection_after_response = true;
      }
      catch (const std::exception &e) {
        BOOST_LOG(error) << "getPerfCurrent: " << e.what();

        write_runtime_error(response, web::StatusCode::internal_server_error, 500, std::string(e.what()));
      }
      catch (...) {
        BOOST_LOG(error) << "getPerfCurrent: Unknown exception";
        write_runtime_error(response, web::StatusCode::internal_server_error, 500, "Unknown error");
      }
    }

    void
    setPerfFrameRates(resp_https_t response, req_https_t request) {
      if (!check_content_type(response, request, "application/json")) return;
      if (!authenticate(response, request)) return;
      if (!require_localhost(response, request, "controlling frame-rate collection")) return;

      try {
        if (request->content.size() > 1024) {
          throw std::invalid_argument("frame-rate subscription body must not exceed 1024 bytes");
        }
        std::stringstream content;
        content << request->content.rdbuf();
        const auto body = json::parse(content.str());
        if (!body.is_object() || !body.contains("consumer_id") || !body["consumer_id"].is_string() ||
            !body.contains("enabled") || !body["enabled"].is_boolean() ||
            (body.contains("lease_ms") && !body["lease_ms"].is_number_integer())) {
          throw std::invalid_argument("expected consumer_id (string), enabled (boolean), and optional lease_ms (integer)");
        }
        const auto lease_ms = body.value("lease_ms", std::int64_t { 30000 });
        if (lease_ms < 1000 || lease_ms > 30000) {
          throw std::invalid_argument("lease_ms must be between 1000 and 30000");
        }
        const auto state = perf::set_frame_rate_subscription(body["consumer_id"].get<std::string>(), body["enabled"].get<bool>(),
          std::chrono::milliseconds { lease_ms });
        response->write(json { { "success", true }, { "frame_rate_collection", state } }.dump(), { { "Content-Type", "application/json" } });
        response->close_connection_after_response = true;
      }
      catch (const json::exception &e) {
        write_runtime_error(response, web::StatusCode::bad_request, 400, e.what());
      }
      catch (const std::invalid_argument &e) {
        write_runtime_error(response, web::StatusCode::bad_request, 400, e.what());
      }
      catch (const std::exception &e) {
        BOOST_LOG(error) << "setPerfFrameRates: " << e.what();
        write_runtime_error(response, web::StatusCode::internal_server_error, 500, e.what());
      }
    }

    void
    changeRuntimeBitrate(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;

      print_req(request);

      if (!require_localhost(response, request, "changing runtime bitrate")) {
        return;
      }

      try {
        auto args = request->parse_query_string();
        auto bitrate_param = args.find("bitrate");
        auto clientname_param = args.find("clientname");

        // 验证参数
        if (bitrate_param == args.end()) {
          std::ostringstream msg_stream;
          msg_stream << "Missing bitrate parameter when changing bitrate";
          BOOST_LOG(warning) << msg_stream.str();
          write_runtime_error(response, web::StatusCode::bad_request, 400, msg_stream.str());
          return;
        }

        if (clientname_param == args.end()) {
          std::ostringstream msg_stream;
          msg_stream << "Missing clientname parameter when changing bitrate";
          BOOST_LOG(warning) << msg_stream.str();
          write_runtime_error(response, web::StatusCode::bad_request, 400, msg_stream.str());
          return;
        }

        // 安全地解析码率参数
        int bitrate = 0;
        try {
          bitrate = std::stoi(bitrate_param->second);
        }
        catch (...) {
          write_runtime_error(response, web::StatusCode::bad_request, 400, "Invalid bitrate parameter format");
          return;
        }

        std::string client_name = clientname_param->second;

        // 验证码率范围
        if (bitrate <= 0 || bitrate > 800000) {
          std::ostringstream msg_stream;
          msg_stream << "Invalid bitrate value when changing bitrate. Must be between 1 and 800000 Kbps";
          BOOST_LOG(warning) << msg_stream.str();
          write_runtime_error(response, web::StatusCode::bad_request, 400, msg_stream.str());
          return;
        }

        // 获取所有活动会话以便调试
        std::vector<std::string> available_clients;
        try {
          auto sessions_info = stream::session::get_all_sessions_info();
          for (const auto &session_info : sessions_info) {
            if (session_info.state == "RUNNING") {
              available_clients.push_back(session_info.client_name);
            }
          }
        }
        catch (...) {
          // 继续执行，即使获取会话信息失败，仍然尝试修改码率
        }

        BOOST_LOG(info) << "Config API: Attempting to change bitrate for client '" << client_name
                        << "' to " << bitrate << " Kbps";
        if (!available_clients.empty()) {
          BOOST_LOG(info) << "Available RUNNING clients: " << boost::algorithm::join(available_clients, ", ");
        }

        // 调用底层 API 修改码率
        video::dynamic_param_t param;
        param.type = video::dynamic_param_type_e::BITRATE;
        param.value.int_value = bitrate;
        param.valid = true;

        bool success = stream::session::change_dynamic_param_for_client(client_name, param);

        json response_json;
        if (success) {
          response_json["success"] = true;
          response_json["status_code"] = 200;
          response_json["status_message"] = "Bitrate change request sent to client session";
          response_json["bitrate"] = bitrate;
          response_json["client_name"] = client_name;

          BOOST_LOG(info) << "Config API: Dynamic bitrate change requested for client '"
                          << client_name << "': " << bitrate << " Kbps";
        }
        else {
          std::string error_msg = "No active streaming session found for client: " + client_name;
          if (!available_clients.empty()) {
            error_msg += ". Available clients: " + boost::algorithm::join(available_clients, ", ");
          }
          else {
            error_msg += ". No RUNNING sessions available.";
          }

          response_json["success"] = false;
          response_json["status_code"] = 404;
          response_json["status_message"] = error_msg;

          BOOST_LOG(warning) << "Config API: Failed to change bitrate - " << error_msg;
        }

        response->write(
          success ? web::StatusCode::ok : web::StatusCode::not_found,
          response_json.dump());
        response->close_connection_after_response = true;
      }
      catch (const std::exception &e) {
        BOOST_LOG(error) << "changeRuntimeBitrate: " << e.what();
        write_runtime_error(response, web::StatusCode::internal_server_error, 500, std::string(e.what()));
      }
      catch (...) {
        BOOST_LOG(error) << "changeRuntimeBitrate: Unknown exception";
        write_runtime_error(response, web::StatusCode::internal_server_error, 500, "Unknown error");
      }
    }

  }  // namespace

  void
  register_routes(https_server_t &server) {
    server.resource["^/api/runtime/sessions$"]["GET"] = getRuntimeSessions;
    server.resource["^/api/runtime/sessions/stop$"]["POST"] = stopRuntimeSessions;
    server.resource["^/api/runtime/hdr$"]["GET"] = getRuntimeHdrStatus;
    server.resource["^/api/runtime/hdr-calibration$"]["GET"] = getRuntimeHdrCalibration;
    server.resource["^/api/runtime/bitrate$"]["GET"] = changeRuntimeBitrate;
    server.resource["^/api/perf/current$"]["GET"] = getPerfCurrent;
    server.resource["^/api/perf/frame-rates$"]["POST"] = setPerfFrameRates;
  }

}  // namespace confighttp::runtime
