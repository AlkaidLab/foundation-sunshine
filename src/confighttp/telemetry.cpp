/**
 * @file src/confighttp/telemetry.cpp
 * @brief Telemetry routes of the Web UI server.
 *
 *   GET  /api/telemetry/identity  {install_id, session_id, endpoint, usage, errors}; 403 while both choices are off
 *   POST /api/telemetry/events    {product: "panel"|"web"|"sparxie-launch", version?, events: [{kind, name, props}]} -> {accepted}
 *   GET  /api/telemetry/status    the two choices, the endpoint and the counters of the sender
 *   GET  /api/telemetry/preview   the events that wait in memory for the next batch, exactly as they would be sent
 *
 * The Panel and the Web UI hand their events to the host instead of sending them themselves: one sender, one identity, one place where the two choices
 * are applied, and a page behind a content blocker (or in a network that does not reach the endpoint) still counts. The pages are served by this host; the
 * request is authenticated like every other call of the Web UI.
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

#include "telemetry.h"

#include "src/confighttp/auth.h"
#include "src/confighttp/common.h"
#include "src/logging.h"
#include "src/telemetry/telemetry.h"

using namespace std::literals;
using json = nlohmann::json;

namespace confighttp::telemetry_api {
  using namespace confighttp;

  namespace {
    constexpr std::size_t MAX_BODY_BYTES = 256 * 1024;

    void
    getIdentity(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;
      if (auto identity = ::telemetry::identity()) {
        write_json(response, web::StatusCode::ok, *identity);
        return;
      }
      // both choices are off: no identity is created or given out
      write_json_error(response, web::StatusCode::forbidden, "telemetry is off");
    }

    void
    postEvents(resp_https_t response, req_https_t request) {
      if (!check_content_type(response, request, "application/json")) return;
      if (!authenticate(response, request)) return;
      if (request->content.size() > MAX_BODY_BYTES) {
        write_json_error(response, web::StatusCode::payload_too_large, "too large");
        return;
      }
      json body;
      if (!read_json_body(response, request, body)) return;
      if (!body.is_object() || !body.contains("product") || !body["product"].is_string() || !body.contains("events") || !body["events"].is_array()) {
        write_json_error(response, web::StatusCode::bad_request, "expected {product, events}");
        return;
      }
      const auto product = body["product"].get<std::string>();
      if (product != "panel" && product != "web" && product != "sparxie-launch") {
        write_json_error(response, web::StatusCode::bad_request, "unknown product");
        return;
      }
      const auto version = body.contains("version") && body["version"].is_string() ? body["version"].get<std::string>() : std::string {};
      // A choice that is off is not an error for the page: it simply keeps nothing, and the page has nothing to retry.
      const auto accepted = ::telemetry::ingest(product, version, body["events"]);
      write_json(response, web::StatusCode::ok, json { { "accepted", accepted } });
    }

    void
    getStatus(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;
      write_json(response, web::StatusCode::ok, ::telemetry::status());
    }

    void
    getPreview(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;
      write_json(response, web::StatusCode::ok, ::telemetry::preview());
    }
  }  // namespace

  void
  register_routes(https_server_t &server) {
    server.resource["^/api/telemetry/identity$"]["GET"] = getIdentity;
    server.resource["^/api/telemetry/events$"]["POST"] = postEvents;
    server.resource["^/api/telemetry/status$"]["GET"] = getStatus;
    server.resource["^/api/telemetry/preview$"]["GET"] = getPreview;
  }
}  // namespace confighttp::telemetry_api
