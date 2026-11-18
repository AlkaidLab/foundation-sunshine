/**
 * @file src/confighttp/doctor.cpp
 * @brief GET /api/doctor: runs the read-only health check, every group or the ones named by ?only=.
 */
#include "doctor.h"

#include <nlohmann/json.hpp>

#include "src/confighttp/auth.h"
#include "src/confighttp/common.h"
#include "src/doctor/doctor.h"
#include "src/doctor/doctor_checks.h"
#include "src/logging.h"
#include "src/rtsp.h"

using namespace std::literals;

namespace confighttp::doctor {
  namespace {
    void
    getDoctor(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;

      const auto query = request->parse_query_string();
      std::vector<std::string> only;
      if (const auto found = query.find("only"); found != query.end()) {
        std::vector<std::string> unknown;
        only = ::doctor::parse_groups(found->second, unknown);
        if (!unknown.empty() || only.empty()) {
          write_json_error(response, web::StatusCode::bad_request, "Unknown group: " + (unknown.empty() ? found->second : unknown.front()));
          return;
        }
      }
      auto lang = ::doctor::lang_e::en;
      if (const auto found = query.find("lang"); found != query.end()) lang = ::doctor::parse_lang(found->second);

      try {
        auto report = ::doctor::run(::doctor::host_checks(), only, lang);
        report.streaming = rtsp_stream::session_starting_or_active();
        BOOST_LOG(info) << "Health check: "sv << report.groups.size() << " groups in "sv << report.elapsed_ms << " ms"sv;
        write_json(response, web::StatusCode::ok, ::doctor::to_json(report));
      }
      catch (const std::exception &error) {
        write_json_error(response, web::StatusCode::internal_server_error, error.what());
      }
    }
  }  // namespace

  void
  register_routes(https_server_t &server) {
    server.resource["^/api/doctor$"]["GET"] = getDoctor;
  }
}  // namespace confighttp::doctor
