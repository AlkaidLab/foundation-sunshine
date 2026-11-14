/**
 * @file src/confighttp/local_auth_token.h
 * @brief Per-process credential for the local Web UI surface.
 */
#pragma once

#include <filesystem>
#include <string_view>

namespace confighttp::local_auth_token {
  // The token is deliberately a separate header from browser cookies.  It is
  // sent only by the local Panel process after it reads the ACL-protected file.
  constexpr std::string_view header_name = "X-Sunshine-Local-Token";

  /** Rotate the credential and publish it with platform-specific permissions. */
  bool
  initialize();

  /**
   * Atomically replace a file using the same restricted permissions as the
   * local authentication token. The caller must pass only non-secret output
   * or one-way verifiers; raw browser tokens must never be persisted.
   */
  bool
  write_restricted_file(const std::filesystem::path &path, std::string_view contents);

  /** Refresh the Windows file ACL when the active console user changes. */
  void
  start_monitor();

  /** Stop the background ACL/token refresh before the HTTP module exits. */
  void
  stop_monitor();

  /** Compare an already parsed header with the current in-memory credential. */
  bool
  matches(std::string_view candidate);
}  // namespace confighttp::local_auth_token
