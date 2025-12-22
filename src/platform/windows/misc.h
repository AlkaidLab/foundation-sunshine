/**
 * @file src/platform/windows/misc.h
 * @brief Miscellaneous declarations for Windows.
 */
#pragma once

#include <chrono>
#include <functional>
#include <string_view>
#include <system_error>
#include <windows.h>
#include <winnt.h>

namespace platf {
  void
  print_status(const std::string_view &prefix, HRESULT status);
  HDESK
  syncThreadDesktop();

  int64_t
  qpc_counter();

  std::chrono::nanoseconds
  qpc_time_difference(int64_t performance_counter1, int64_t performance_counter2);

  /**
   * @brief Convert a UTF-8 string into a UTF-16 wide string.
   * @param string The UTF-8 string.
   * @return The converted UTF-16 wide string.
   */
  std::wstring
  from_utf8(const std::string &string);

  /**
   * @brief Convert a UTF-16 wide string into a UTF-8 string.
   * @param string The UTF-16 wide string.
   * @return The converted UTF-8 string.
   */
  std::string
  to_utf8(const std::wstring &string);

  /**
   * @brief Check if the current process is running as SYSTEM.
   * @return true if running as SYSTEM, false otherwise.
   */
  bool
  is_running_as_system();

  /**
   * @brief Retrieve the current logged-in user's token.
   * @param elevated Whether to retrieve an elevated token if available.
   * @return The user's token handle, or nullptr if not available.
   */
  HANDLE
  retrieve_users_token(bool elevated);

  /**
   * @brief Impersonate the current user and execute a callback.
   * @param user_token The user's token to impersonate.
   * @param callback The callback function to execute while impersonating.
   * @return Error code, if any.
   */
  std::error_code
  impersonate_current_user(HANDLE user_token, std::function<void()> callback);

  /**
   * @brief Check if a character sequence appears in order in a string (fuzzy matching).
   * @param text The text to search in.
   * @param pattern The pattern to find (characters must appear in order, but can have gaps).
   * @return true if pattern is found, false otherwise.
   */
  bool
  fuzzy_match(const std::wstring &text, const std::wstring &pattern);

  /**
   * @brief Split a string into words (by spaces and common separators).
   * @param text The text to split.
   * @return Vector of words.
   */
  std::vector<std::wstring>
  split_words(const std::wstring &text);
}  // namespace platf
