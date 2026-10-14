/**
 * @file src/confighttp/common.cpp
 * @brief Web UI common handlers and routes.
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

#include "common.h"
#include "src/http_util.h"
#include "src/logging.h"
#include <array>
#include <boost/property_tree/xml_parser.hpp>
#include <sstream>

using namespace std::literals;
using json = nlohmann::json;

namespace confighttp {
  namespace pt = boost::property_tree;

  void
  print_req(const req_https_t &request) {
    std::ostringstream log_stream;
    log_stream << "Request - TUNNEL: HTTPS"
               << ", METHOD: " << request->method
               << ", PATH: " << request->path;

    // Headers stay disabled because authentication and proxy headers may
    // contain credentials.
    /*
    // Headers
    if (!request->header.empty()) {
      log_stream << ", HEADERS: ";
      bool first = true;
      for (auto &[name, val] : request->header) {
        if (!first) log_stream << ", ";
        log_stream << name << "=" << (name == "Authorization" ? "CREDENTIALS REDACTED" : val);
        first = false;
      }
    }

    */

    static constexpr std::array safe_query_parameters {
      "bitrate"sv,
      "clientname"sv,
    };
    auto query_params = request->parse_query_string();
    http_util::append_allowed_request_log_fields(
      log_stream,
      ", PARAMS: "sv,
      query_params,
      safe_query_parameters,
      "&"sv);

    BOOST_LOG(verbose) << log_stream.str();
  }

  /**
   * @brief Send a response.
   * @param response The HTTP response object.
   * @param output_tree The JSON tree to send.
   */
  void
  send_response(resp_https_t response, const nlohmann::json &output_tree) {
    web::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(output_tree.dump(), headers);
  }

  /**
   * @brief Validate the request content type and send bad request when mismatch.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @param contentType The expected content type.
   * @return true if the request's Content-Type matches the expected type.
   *
   * Backport of upstream LizardByte/Sunshine 738ac93a0ec1 (CVE-2025-53095).
   * AlkaidLab does not have upstream's bad_request() helper, so the bad-request
   * response is inlined here.
   */
  bool
  check_content_type(resp_https_t response, req_https_t request, const std::string &contentType) {
    auto send_bad_request = [response](const std::string &error_message) {
      nlohmann::json tree;
      tree["status_code"] = 400;
      tree["status"] = false;
      tree["error"] = error_message;
      web::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "application/json");
      response->write(web::StatusCode::bad_request, tree.dump(), headers);
    };
    auto requestContentType = request->header.find("content-type");
    if (requestContentType == request->header.end()) {
      send_bad_request("Content type not provided");
      return false;
    }
    if (!http_util::content_type_matches(requestContentType->second, contentType)) {
      send_bad_request("Content type mismatch");
      return false;
    }
    return true;
  }

  void
  not_found(resp_https_t response, req_https_t request) {
    pt::ptree tree;
    tree.put("root.<xmlattr>.status_code", 404);

    std::ostringstream data;

    pt::write_xml(data, tree);
    response->write(web::StatusCode::not_found, data.str());
  }

  void
  close_connection(resp_https_t response, req_https_t request) {
    response->close_connection_after_response = true;
    response->write(static_cast<web::StatusCode>(444));
    return;
  }

  web::CaseInsensitiveMultimap
  json_headers() {
    return {
      { "Content-Type", "application/json" },
      { "Cache-Control", "no-store" },
      { "X-Content-Type-Options", "nosniff" }
    };
  }

  void
  write_json(resp_https_t response, web::StatusCode status, const json &body) {
    response->write(status, body.dump(), json_headers());
  }

  void
  write_json_error(resp_https_t response, web::StatusCode status, std::string error) {
    json body;
    body["ok"] = false;
    body["error"] = std::move(error);
    write_json(std::move(response), status, body);
  }

  bool
  read_json_body(resp_https_t response, req_https_t request, json &body) {
    std::stringstream ss;
    ss << request->content.rdbuf();
    try {
      body = json::parse(ss.str());
      return true;
    }
    catch (const std::exception &e) {
      write_json_error(std::move(response), web::StatusCode::bad_request, std::string { "Invalid JSON: " } + e.what());
      return false;
    }
  }

  void
  write_runtime_error(resp_https_t response, web::StatusCode http_status, int status_code, const std::string &status_message) {
    json error_json;
    error_json["success"] = false;
    error_json["status_code"] = status_code;
    error_json["status_message"] = status_message;

    response->write(http_status, error_json.dump());
    response->close_connection_after_response = true;
  }

}  // namespace confighttp
