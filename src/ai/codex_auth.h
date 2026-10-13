/**
 * @file src/ai/codex_auth.h
 * @brief OpenAI Codex device-code sign-in for the AI assistant.
 */
#pragma once

#include "credential_store.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace codex_auth {

  struct start_result_t {
    bool success = false;
    std::string user_code;
    std::string verification_uri;
    int interval_seconds = 0;
    int expires_in_seconds = 0;
    std::string error;
    std::string flow_id;
  };

  enum class poll_state_e {
    pending,
    complete,
    expired,
    retryable_error,
    error
  };

  struct poll_result_t {
    poll_state_e state = poll_state_e::error;
    int retry_after_seconds = 0;
    std::string error;
  };

  struct status_result_t {
    bool connected = false;
    std::string account_id;
    std::string error;
    bool pending = false;
    std::string user_code;
    std::string verification_uri;
    std::string flow_id;
    int interval_seconds = 0;
  };

  struct token_result_t {
    bool success = false;
    std::string access_token;
    std::string account_id;
    std::string error;
  };

  // These functions perform blocking network I/O. Call them off the UI thread.
  // Pass <config directory>/ai_codex_credential.bin to every path-taking call.
  start_result_t start();
  poll_result_t poll(const std::filesystem::path &credential_path, std::string_view flow_id);
  status_result_t status(const std::filesystem::path &credential_path);
  credential_store::mutation_result_t logout(const std::filesystem::path &credential_path);
  token_result_t access_token(const std::filesystem::path &credential_path);

  // Exposed for validation and a focused offline test; never treats a JWT as verified.
  std::string account_id_from_jwt(std::string_view access_token);

  namespace detail {
    enum class poll_response_e { authorized, pending, slow_down, error };
    poll_response_e classify_device_poll_response(long http_status, std::string_view body);
  }  // namespace detail

}  // namespace codex_auth
