/**
 * @file src/network_external.cpp
 * @brief The outside check of a public address (see network_external.h).
 */
#include "network_external.h"

#include <algorithm>

#include <nlohmann/json.hpp>

namespace network_external {
  std::string
  build_request(const request_t &request) {
    nlohmann::json ports = nlohmann::json::array();
    for (const auto &entry : request.ports) ports.push_back({ { "role", entry.role }, { "port", entry.port } });
    return nlohmann::json {
      { "v", 1 },
      { "install_id", request.install_id },
      { "target", request.target },
      { "uniqueid", request.uniqueid },
      { "ports", ports },
    }.dump();
  }

  result_t
  parse_response(int status, long retry_after_s, const std::string &body) {
    result_t result;
    result.retry_after_s = retry_after_s;
    if (status == 0) {
      result.reason = "no_answer";
      return result;
    }
    nlohmann::json json = nlohmann::json::parse(body, nullptr, false);
    if (status == 429) {
      result.reason = "rate_limited";
      return result;
    }
    if (status == 403 && json.is_object() && json.value("error", "") == "target_not_caller") {
      result.availability = availability_e::refused;
      result.reason = json.value("reason", "ip_mismatch");
      return result;
    }
    if (status == 503 || status == 404) {
      result.reason = "disabled";  // the service is off or does not have the route
      return result;
    }
    if (status == 400) {
      result.reason = "bad_request";
      return result;
    }
    if (status != 200 || !json.is_object() || json.value("v", 0) != 1 || !json.contains("tcp") || !json["tcp"].is_array()) {
      result.reason = "bad_answer";
      return result;
    }
    for (const auto &entry : json["tcp"]) {
      if (!entry.is_object()) continue;
      tcp_port_t port;
      port.role = entry.value("role", "");
      port.port = static_cast<std::uint16_t>(entry.value("port", 0));
      port.state = entry.value("state", "");
      port.identity = entry.value("identity", "unverified");
      port.ms = entry.value("ms", 0);
      if (port.role.empty() || port.state.empty()) continue;
      result.tcp.push_back(std::move(port));
    }
    if (result.tcp.empty()) {
      result.reason = "bad_answer";
      return result;
    }
    result.availability = availability_e::answered;
    return result;
  }

  result_t
  probe(const std::string &endpoint, const request_t &request, const post_fn &post) {
    std::string answer;
    long retry_after = 0;
    const auto status = post(endpoint + "/v1/reach-probe", build_request(request), answer, retry_after);
    return parse_response(status, retry_after, answer);
  }

  std::string
  tcp_summary(const result_t &result) {
    if (result.availability != availability_e::answered) return "unknown";
    const auto http = std::find_if(result.tcp.begin(), result.tcp.end(), [](const tcp_port_t &entry) { return entry.role == "http"; });
    if (http != result.tcp.end()) {
      if (http->state == "open" && http->identity == "match") return "reachable";
      if (http->state == "open" && http->identity == "mismatch") return "another_host";
      if (http->state == "open") return "unknown";  // answered, but not as a GameStream host: a router's page, something else
      return http->state == "timeout" ? "timeout" : "closed";
    }
    return std::any_of(result.tcp.begin(), result.tcp.end(), [](const tcp_port_t &entry) { return entry.state == "open"; }) ? "unknown" : "closed";
  }

  bool
  enabled(std::string_view setting) {
    return setting == "enabled";
  }
}  // namespace network_external
