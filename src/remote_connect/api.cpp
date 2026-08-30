#include "api.h"

#include <utility>

#include <nlohmann/json.hpp>

#include "service.h"

namespace remote_connect::api {
  namespace {
    using json = nlohmann::json;

    SimpleWeb::CaseInsensitiveMultimap
    json_headers() {
      return {
        { "Content-Type", "application/json" },
        { "X-Frame-Options", "DENY" },
        { "Content-Security-Policy", "frame-ancestors 'none';" },
      };
    }

    void
    write_json(resp_https_t response, const json &body) {
      response->write(body.dump(), json_headers());
    }

    json
    status_response(bool success, const status_t &remote_status) {
      return {
        { "status", success },
        { "enabled", remote_status.enabled },
        { "running", remote_status.running },
        { "available", remote_status.available },
        { "virtual_ip", remote_status.virtual_ip },
        { "error", remote_status.error },
      };
    }

  }  // namespace

  void
  get_status(resp_https_t response) {
    write_json(std::move(response), status_response(true, remote_connect::status()));
  }

  void
  set_enabled(resp_https_t response, req_https_t request) {
    json input;
    try {
      input = json::parse(request->content.string());
    }
    catch (const json::exception &e) {
      write_json(std::move(response), { { "status", false }, { "error", e.what() } });
      return;
    }

    if (!input.is_object() || !input.contains("enabled") || !input["enabled"].is_boolean()) {
      write_json(std::move(response), { { "status", false }, { "error", "enabled must be a boolean" } });
      return;
    }

    const auto result = remote_connect::set_enabled(input["enabled"].get<bool>());
    write_json(std::move(response), status_response(result.success, result.status));
  }

  void
  reset_enrollment(resp_https_t response) {
    const auto result = remote_connect::reset_enrollment();
    write_json(std::move(response), status_response(result.success, result.status));
  }

}  // namespace remote_connect::api
