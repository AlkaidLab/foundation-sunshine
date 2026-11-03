/**
 * @file src/nvhttp/pairing_pending.h
 * @brief Pending-PIN registry for the pairing session.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "src/nvhttp.h"

namespace nvhttp::pairing_pending {
  using resp_http_t = std::shared_ptr<typename web::ServerBase<web::HTTP>::Response>;
  using resp_https_t = std::shared_ptr<typename web::ServerBase<SunshineHTTPS>::Response>;
  using response_t = util::Either<resp_http_t, resp_https_t>;

  enum class add_result_e {
    added,
    rate_limited,
  };

  struct taken_t {
    std::string unique_id;
    std::string request_id;
    std::string device_name;
    response_t response;
  };

  /// Add a pending request. The response is moved only when the request is
  /// accepted, so a rate-limited caller can still write its own error.
  bool
  can_add(const std::string &source_address);

  add_result_e
  add(
    const std::string &unique_id,
    std::string_view device_name,
    const std::string &source_address,
    response_t &response);

  /// True when this client already has a live pending request.
  bool
  has_pending(const std::string &unique_id);

  /// Remove by client uniqueid. The waiting client receives a 400 if it is
  /// still pending.
  void
  remove(const std::string &unique_id);

  /// Take the request by opaque id, or the oldest when id is empty.
  std::optional<taken_t>
  take(const std::string &request_id);

  /// Reject by opaque id and answer the waiting client with 403.
  bool
  reject(const std::string &request_id);

  /// Current live snapshot with its generation cursor.
  pending_pair_snapshot_t
  snapshot();

  /// Wait for the generation cursor to change or the timeout to elapse.
  bool
  wait(std::uint64_t generation, std::chrono::milliseconds timeout);

  /// Write a fixed XML rejection/timeout to a taken response.
  void
  send_xml(response_t &response, int status_code, const std::string &status_message);

  /// Write an already-built XML document to a taken response.
  void
  write_xml(response_t &response, const std::string &xml);

  /// Display-safe device name: valid UTF-8, no C0/C1 or bidi controls,
  /// collapsed whitespace, at most 64 bytes.
  std::string
  sanitize_device_name(std::string_view name);

  /// Masked identifier for logs.
  std::string
  mask_for_log(std::string_view value);

#ifdef SUNSHINE_TESTS
  void
  set_timeout_for_test(std::chrono::milliseconds timeout);
  void
  reset_for_test();
#endif
}  // namespace nvhttp::pairing_pending
