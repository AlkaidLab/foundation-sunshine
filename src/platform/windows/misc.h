/**
 * @file src/platform/windows/misc.h
 * @brief Miscellaneous declarations for Windows.
 */
#pragma once

#include <mutex>

#include <chrono>
#include <functional>
#include <string>
#include <string_view>
#include <system_error>
#include <windows.h>
#include <winnt.h>

namespace platf {
  /** Keep HKCU sensitive to the impersonated user's token. */
  bool
  initialize_user_registry();

  /** Serializes WCS reads with legacy process-wide Shell registry overrides. */
  std::mutex &
  user_registry_mutex();
  void
  print_status(const std::string_view &prefix, HRESULT status);
  HDESK
  syncThreadDesktop();

  int64_t
  qpc_counter();

  /**
   * @brief Convert a QueryPerformanceCounter tick delta to nanoseconds.
   * @param ticks The signed difference between two QPC readings.
   * @param frequency The QueryPerformanceFrequency value in ticks per second.
   * @return The equivalent signed duration, or zero if frequency is invalid.
   */
  std::chrono::nanoseconds
  qpc_ticks_to_duration(int64_t ticks, int64_t frequency);

  std::chrono::nanoseconds
  qpc_time_difference(int64_t performance_counter1, int64_t performance_counter2);

  /**
   * @brief Called by the text encoding helpers when a conversion fails.
   * The helpers live in text_encoding.cpp, which small host tools link without the
   * logging sources; the host binaries install a logging callback in misc.cpp.
   */
  extern void (*text_encoding_failure_hook)(const char *what, DWORD error);

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
   * @brief Convert a byte string in a named code page into UTF-8.
   * @param text The source bytes.
   * @param code_page A Windows code page (CP_ACP, CP_OEMCP, CP_UTF8 or a number such as 936).
   * @return The UTF-8 text, or an empty string when the input cannot be converted.
   */
  std::string
  to_utf8(std::string_view text, UINT code_page);

  /**
   * @brief Keep valid UTF-8 as it is; convert anything else from a legacy code page.
   * @param text The source bytes, for example a third-party tool's output.
   * @param fallback_code_page The code page to use when the bytes are not valid UTF-8.
   * @return UTF-8 text. Invalid input is replaced with U+FFFD rather than dropped.
   */
  std::string
  to_utf8_best_effort(std::string_view text, UINT fallback_code_page = CP_ACP);

  /** True when the bytes are a valid UTF-8 sequence. */
  bool
  is_valid_utf8(std::string_view text);

  /**
   * @brief Format a Win32 error or HRESULT with the system message table, as UTF-8.
   * @param message_id The error code (for example GetLastError() or an HRESULT).
   * @param language_id The message language.
   * @return The message without trailing line breaks; a code-only fallback when the table has none.
   */
  std::string
  format_message_utf8(DWORD message_id, DWORD language_id = MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT));

  /**
   * @brief Check if the current process is running as SYSTEM.
   * @return true if running as SYSTEM, false otherwise.
   */
  bool
  is_running_as_system();

  /**
   * @brief 返回操作系统报告的 Windows 版本。
   * @return Windows 版本字符串；无法获取时返回 "unknown"。
   */
  std::string
  windows_version();

  /**
   * @brief Launch the bundled GUI user agent in hidden mode.
   * @return An error code when the agent cannot be started.
   */
  std::error_code
  launch_gui_agent();

  /**
   * @brief Start the bundled GUI agent and return a process handle.
   * @param ec Set when the agent cannot be started. Cleared on success.
   * @return A handle with SYNCHRONIZE and PROCESS_QUERY_LIMITED_INFORMATION,
   *         or nullptr. The caller owns the handle. The process is not killed
   *         when the handle is closed.
   */
  HANDLE
  launch_gui_agent_process(std::error_code &ec);

  /**
   * @brief Watch the bundled GUI agent and start it again when it disappears
   *        without a deliberate menu quit. Used when Core is not the service.
   */
  void
  supervise_gui_agent();

  /**
   * @brief Stop watching. Does not terminate a running agent.
   */
  void
  stop_gui_agent_supervisor();

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
