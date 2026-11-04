/**
 * @file src/confighttp/systeminfo.cpp
 * @brief Authenticated CPU, GPU, adapter, OS and host name inventory for the
 *        Web UI home tiles, and the build identity of the host and the start time of its process (GET /api/version).
 */
#include "systeminfo.h"

#include "src/build_info.h"
#include "src/config.h"
#include "src/confighttp/auth.h"
#include "src/confighttp/common.h"
#include "src/logging.h"
#include "src/system_info.h"

using namespace std::literals;

namespace confighttp::systeminfo {
  namespace {
    void
    getSystemInfo(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;

      bool refresh = false;
      const auto query = request->parse_query_string();
      const auto found = query.find("refresh");
      if (found != query.end()) refresh = found->second == "1" || found->second == "true";

      try {
        const auto snapshot = ::system_info::sample(config::video.adapter_name, refresh);
        BOOST_LOG(debug) << "System info: "sv << snapshot.nics.size() << " adapters, "sv << snapshot.gpus.size() << " GPUs"sv;
        write_json(response, web::StatusCode::ok, ::system_info::to_json(snapshot));
      }
      catch (const std::exception &error) {
        write_json_error(response, web::StatusCode::internal_server_error, error.what());
      }
    }

    // The build identity (build_info.h): full commit, channel, UTC build time, variant, package. Authenticated like the
    // rest of the API: the panel reads the same fields from the VERSIONINFO of sunshine.exe without a request.
    void
    getVersion(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;
      auto body = ::build_info::to_json(::build_info::current());
      body["process_started_at"] = ::build_info::process_started_at_utc();
      write_json(response, web::StatusCode::ok, body);
    }
  }  // namespace

  void
  register_routes(https_server_t &server) {
    server.resource["^/api/system/info$"]["GET"] = getSystemInfo;
    server.resource["^/api/version$"]["GET"] = getVersion;
  }
}  // namespace confighttp::systeminfo
