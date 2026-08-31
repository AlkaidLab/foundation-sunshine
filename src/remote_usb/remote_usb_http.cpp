/**
 * @file src/remote_usb/remote_usb_http.cpp
 * @brief Capability response helpers for the Remote USB HTTP endpoint.
 */

#include "remote_usb_http.h"

#include <charconv>
#include <limits>

#include <nlohmann/json.hpp>

namespace remote_usb_http {
namespace {

SimpleWeb::CaseInsensitiveMultimap
json_headers() {
  return {
    { "Content-Type", "application/json" },
    { "Cache-Control", "no-store" },
    { "Pragma", "no-cache" },
    { "X-Content-Type-Options", "nosniff" }
  };
}

}  // namespace

std::optional<std::uint64_t>
parse_stream_generation(std::string_view value) {
  if (value.empty() || value.size() > 20) {
    return std::nullopt;
  }
  /* from_chars rejects signs/whitespace and reports overflow without invoking
   * locale-dependent parsing. */
  std::uint64_t parsed = 0;
  const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed, 10);
  if (result.ec != std::errc {} || result.ptr != value.data() + value.size() || parsed == 0) {
    return std::nullopt;
  }
  return parsed;
}

std::optional<std::uint64_t>
parse_nonzero_token(std::string_view value) {
  /* Token query values intentionally use the same strict grammar as the
   * generation: decimal digits only, bounded to uint64_t, and never zero. */
  return parse_stream_generation(value);
}

std::optional<capability_request>
parse_capability_request(std::string_view stream_generation, std::string &error) {
  error.clear();
  const auto parsed = parse_stream_generation(stream_generation);
  if (!parsed) {
    error = "stream_generation must be a non-zero unsigned decimal integer";
    return std::nullopt;
  }
  return capability_request { *parsed, 0, 0, 0 };
}

std::optional<capability_request>
parse_capability_request(std::string_view stream_generation,
                         std::string_view session_token,
                         std::string_view attachment_token,
                         std::string_view lease_token,
                         std::string &error) {
  error.clear();
  const auto generation = parse_stream_generation(stream_generation);
  if (!generation) {
    error = "stream_generation must be a non-zero unsigned decimal integer";
    return std::nullopt;
  }
  const auto session = parse_nonzero_token(session_token);
  if (!session) {
    error = "session_token must be a non-zero unsigned decimal integer";
    return std::nullopt;
  }
  const auto attachment = parse_nonzero_token(attachment_token);
  if (!attachment) {
    error = "attachment_token must be a non-zero unsigned decimal integer";
    return std::nullopt;
  }
  const auto lease = parse_nonzero_token(lease_token);
  if (!lease) {
    error = "lease_token must be a non-zero unsigned decimal integer";
    return std::nullopt;
  }
  return capability_request { *generation, *session, *attachment, *lease };
}

http_response
make_capability_response(const remote_usb::capability &capability) {
  nlohmann::json body {
    { "version", 1 },
    { "endpoint", {
        { "host", capability.endpoint.host },
        { "port", capability.endpoint.port }
      } },
    { "nonce", remote_usb::capability_store::encode_nonce(capability.nonce) },
    { "maxUrb", capability.max_urb },
    { "maxInflight", capability.max_inflight },
    { "expiresMs", capability.expires_ms }
  };
  return {
    SimpleWeb::StatusCode::success_ok,
    body.dump(),
    json_headers()
  };
}

http_response
make_error_response(SimpleWeb::StatusCode status,
                    std::string_view code,
                    std::string_view message) {
  nlohmann::json body {
    { "version", 1 },
    { "error", std::string { code } }
  };
  if (!message.empty()) {
    body["message"] = std::string { message };
  }
  return { status, body.dump(), json_headers() };
}

}  // namespace remote_usb_http
