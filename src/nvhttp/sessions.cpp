#include "sessions.h"

#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "src/logging.h"
#include "src/network.h"
#include "src/rtsp.h"
#include "src/stream.h"

using json = nlohmann::json;
using namespace std::literals;

namespace nvhttp::sessions {

  namespace {

    void
    log_request(const req_https_t &request) {
      BOOST_LOG(debug) << "Request - Protocol: HTTPS"
                       << ", IP: " << request->remote_endpoint().address().to_string()
                       << ", PORT: " << request->remote_endpoint().port()
                       << ", METHOD: " << request->method
                       << ", PATH: " << request->path;
    }

  }  // namespace

  void
  get(resp_https_t response, req_https_t request) {
    log_request(request);

    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");

    auto client_address = request->remote_endpoint().address();
    auto address = net::addr_to_normalized_string(client_address);
    auto ip_type = net::from_address(address);
    if (ip_type != net::PC) {
      json response_json;
      response_json["success"] = false;
      response_json["status_code"] = 403;
      std::ostringstream msg_stream;
      msg_stream << "Access denied. Only localhost requests are allowed. Client IP: " << client_address.to_string();
      response_json["status_message"] = msg_stream.str();

      response->write(SimpleWeb::StatusCode::client_error_forbidden, response_json.dump(), headers);
      response->close_connection_after_response = true;
      return;
    }

    try {
      auto sessions_info = stream::session::get_all_sessions_info();

      json response_json;
      response_json["success"] = true;
      response_json["status_code"] = 200;
      response_json["status_message"] = "Success";
      response_json["total_sessions"] = sessions_info.size();
      response_json["pending_sessions"] = rtsp_stream::pending_session_count();

      json sessions_array = json::array();

      for (const auto &session_info : sessions_info) {
        json session_obj;
        session_obj["client_name"] = session_info.client_name;
        session_obj["client_address"] = session_info.client_address;
        session_obj["state"] = session_info.state;
        session_obj["session_id"] = session_info.session_id;
        session_obj["width"] = session_info.width;
        session_obj["height"] = session_info.height;
        session_obj["fps"] = session_info.fps;
        session_obj["host_audio"] = session_info.host_audio;
        session_obj["enable_hdr"] = session_info.enable_hdr;
        session_obj["enable_mic"] = session_info.enable_mic;
        session_obj["app_name"] = session_info.app_name;
        session_obj["app_id"] = session_info.app_id;

        sessions_array.push_back(session_obj);
      }

      response_json["sessions"] = sessions_array;

      BOOST_LOG(info) << "NVHTTP API: Session info requested from localhost, returned " << sessions_info.size() << " sessions";

      response->write(SimpleWeb::StatusCode::success_ok, response_json.dump(), headers);
      response->close_connection_after_response = true;
    }
    catch (std::exception &e) {
      BOOST_LOG(warning) << "GetSessionsInfo: "sv << e.what();

      json error_json;
      error_json["success"] = false;
      error_json["status_code"] = 500;
      error_json["status_message"] = e.what();

      response->write(SimpleWeb::StatusCode::server_error_internal_server_error, error_json.dump(), headers);
      response->close_connection_after_response = true;
    }
  }

}  // namespace nvhttp::sessions
