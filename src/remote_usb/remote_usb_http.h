/**
 * @file src/remote_usb/remote_usb_http.h
 * @brief Capability response helpers for the Remote USB HTTP endpoint.
 */
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include <Simple-Web-Server/server_http.hpp>

#include "remote_usb_capability.h"

namespace remote_usb_http {

struct http_response {
  SimpleWeb::StatusCode status { SimpleWeb::StatusCode::success_ok };
  std::string body;
  SimpleWeb::CaseInsensitiveMultimap headers;
};

struct capability_request {
  std::uint64_t stream_generation { 0 };
  /** Tokens are zero only for the legacy parser overload. */
  std::uint64_t session_token { 0 };
  std::uint64_t attachment_token { 0 };
  std::uint64_t lease_token { 0 };
};

/** Parse only an unsigned decimal stream generation (no sign/whitespace). */
std::optional<std::uint64_t> parse_stream_generation(
  std::string_view value);

/** Parse the endpoint query and return a stable error string on failure. */
std::optional<capability_request> parse_capability_request(
  std::string_view stream_generation,
  std::string &error);

/**
 * Parse a capability request including the complete lease binding.  Every
 * token is a strict non-zero unsigned decimal value; signs, whitespace,
 * duplicate/blank query values, and overflow are rejected by the route before
 * a capability is issued.
 */
std::optional<capability_request> parse_capability_request(
  std::string_view stream_generation,
  std::string_view session_token,
  std::string_view attachment_token,
  std::string_view lease_token,
  std::string &error);

/** Build the fixed v1 capability JSON response. */
http_response make_capability_response(
  const remote_usb::capability &capability);

/** Build a JSON error response without leaking nonce/token material. */
http_response make_error_response(
  SimpleWeb::StatusCode status,
  std::string_view code,
  std::string_view message = {});

}  // namespace remote_usb_http
