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

  namespace {
    result_t
    parse_checked(int status, long retry_after_s, const std::string &body);

    bool
    known_state(const std::string &state) {
      return state == "open" || state == "closed" || state == "timeout" || state == "blocked";
    }
  }  // namespace

  result_t
  parse_response(int status, long retry_after_s, const std::string &body) {
    // The service is outside this process: whatever it answers, a malformed answer is "bad_answer" and never an exception.
    try {
      return parse_checked(status, retry_after_s, body);
    }
    catch (const std::exception &) {
      result_t result;
      result.retry_after_s = retry_after_s;
      result.reason = "bad_answer";
      return result;
    }
  }

  namespace {
  result_t
  parse_checked(int status, long retry_after_s, const std::string &body) {
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
    if (status == 403 && json.is_object() && json.contains("error") && json["error"].is_string() && json["error"].get<std::string>() == "target_not_caller") {
      result.availability = availability_e::refused;
      result.reason = json.contains("reason") && json["reason"].is_string() ? json["reason"].get<std::string>() : "ip_mismatch";
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
    const auto bad = [&result]() {
      result.tcp.clear();
      result.reason = "bad_answer";
      return result;
    };
    if (status != 200 || !json.is_object() || !json.contains("v") || !json["v"].is_number_integer() || json["v"].get<long long>() != 1 || !json.contains("tcp") || !json["tcp"].is_array()) {
      return bad();
    }
    for (const auto &entry : json["tcp"]) {
      if (!entry.is_object()) continue;
      // Every field that the verdict reads must be what the protocol says it is: a state it does not know is no evidence, not a closed port.
      if (!entry.contains("role") || !entry["role"].is_string() || !entry.contains("state") || !entry["state"].is_string()) return bad();
      if (!entry.contains("port") || !entry["port"].is_number_unsigned() || entry["port"].get<std::uint64_t>() > 65535) return bad();
      tcp_port_t port;
      port.identity = "unverified";
      port.role = entry["role"].get<std::string>();
      port.state = entry["state"].get<std::string>();
      port.port = static_cast<std::uint16_t>(entry["port"].get<std::uint64_t>());
      if (entry.contains("identity")) {
        if (!entry["identity"].is_string()) return bad();
        port.identity = entry["identity"].get<std::string>();
      }
      if (entry.contains("ms")) {
        if (!entry["ms"].is_number()) return bad();
        port.ms = entry["ms"].get<long long>();
      }
      if (port.role.empty() || !known_state(port.state)) return bad();
      result.tcp.push_back(std::move(port));
    }
    if (result.tcp.empty()) return bad();
    result.availability = availability_e::answered;
    return result;
  }
  }  // namespace

  result_t
  probe(const std::string &endpoint, const request_t &request, const post_fn &post) {
    std::string answer;
    long retry_after = 0;
    int status = 0;
    try {
      status = post(endpoint + "/v1/reach-probe", build_request(request), answer, retry_after);
    }
    catch (const std::exception &) {
      status = 0;  // a request that could not be made is "no answer"
    }
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
