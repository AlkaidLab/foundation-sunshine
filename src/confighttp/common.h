/**
 * @file src/confighttp/common.h
 * @brief Shared Web UI request and response helpers.
 */
#pragma once

#include "types.h"
#include <nlohmann/json.hpp>
#include <string>

namespace confighttp {
  void
  print_req(const req_https_t &request);
  void
  send_response(resp_https_t response, const nlohmann::json &output_tree);
  bool
  check_content_type(resp_https_t response, req_https_t request, const std::string &contentType);
  void
  not_found(resp_https_t response, req_https_t request);
  void
  close_connection(resp_https_t response, req_https_t request);
  web::CaseInsensitiveMultimap
  json_headers();
  void
  write_json(resp_https_t response, web::StatusCode status, const nlohmann::json &body);
  void
  write_json_error(resp_https_t response, web::StatusCode status, std::string error);
  bool
  read_json_body(resp_https_t response, req_https_t request, nlohmann::json &body);
  void
  write_runtime_error(resp_https_t response, web::StatusCode http_status, int status_code, const std::string &status_message);
}  // namespace confighttp
