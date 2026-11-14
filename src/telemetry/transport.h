/**
 * @file src/telemetry/transport.h
 * @brief One blocking HTTP(S) POST with deadlines, over the host's Beast client; used by the sender thread and the crash uploader.
 */
#pragma once

#include <string>

namespace telemetry {

  /** The answer of one POST: the HTTP status (0: nothing answered) and the seconds a 429 or 503 asked to wait (Retry-After; 0: not given). */
  struct post_result_t {
    int status = 0;
    long retry_after_s = 0;
    post_result_t() = default;
    post_result_t(int status_, long retry_after = 0):  // NOLINT: an int is a status
        status(status_), retry_after_s(retry_after) {}
  };

  /**
   * POST `body` to `url` ("http://host:port/path" or "https://host/path"). TLS is verified against the system trust store.
   * @return the HTTP status, 0 when nothing answered (no route, timeout, refused, TLS failure, a bad URL), and the Retry-After of the answer. Never throws.
   */
  post_result_t post_once(const std::string &url, const std::string &content_type, const std::string &body, const std::string &user_agent, long timeout_s = 10, long connect_timeout_s = 5);

}  // namespace telemetry
