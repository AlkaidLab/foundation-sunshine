/**
 * @file src/telemetry/transport.cpp
 * @brief One blocking HTTP(S) POST with deadlines.
 */
#include "transport.h"

#include <algorithm>

#include "src/http/client.h"

namespace telemetry {

  post_result_t
  post_once(const std::string &url, const std::string &content_type, const std::string &body, const std::string &user_agent, long timeout_s, long connect_timeout_s) {
    try {
      const auto scheme_end = url.find("://");
      if (scheme_end == std::string::npos) return {};
      const bool tls = url.compare(0, scheme_end, "https") == 0;
      if (!tls && url.compare(0, scheme_end, "http") != 0) return {};
      const auto authority_start = scheme_end + 3;
      const auto path_start = url.find('/', authority_start);
      const std::string authority = url.substr(authority_start, path_start == std::string::npos ? std::string::npos : path_start - authority_start);
      const std::string target = path_start == std::string::npos ? "/" : url.substr(path_start);
      web::CaseInsensitiveMultimap headers {
        { "Content-Type", content_type },
        { "User-Agent", user_agent },
        { "Accept", "application/json" },
      };
      const auto run = [&](auto &client) {
        client.config.timeout = timeout_s;
        client.config.timeout_connect = connect_timeout_s;
        client.config.max_response_streambuf_size = 64 * 1024;
        auto response = client.request("POST", target, body, headers);
        post_result_t result;
        if (!response) return result;
        try {
          result.status = std::stoi(response->status_code);
          if (const auto found = response->header.find("Retry-After"); found != response->header.end()) result.retry_after_s = std::clamp(std::stol(found->second), 0L, 86400L);
        }
        catch (...) {
          // a Retry-After given as a date, or none: the backoff decides
        }
        return result;
      };
      if (tls) {
        web::Client<web::HTTPS> client(authority, true);
        return run(client);
      }
      web::Client<web::HTTP> client(authority);
      return run(client);
    }
    catch (...) {
      return {};
    }
  }

}  // namespace telemetry
