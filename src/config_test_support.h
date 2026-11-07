/**
 * @file src/config_test_support.h
 * @brief Test-only controls for configuration contention and command-line policy.
 */
#pragma once

#ifdef SUNSHINE_TESTS
  #include <functional>
  #include <string>
  #include <unordered_map>

namespace config::test_support {
  /** Observe an update only after its attempt to acquire the save mutex failed. */
  void
  set_save_lock_contended_hook(std::function<void()> hook);

  /** Replace startup overrides in isolated tests and return the previous policy for restoration. */
  std::unordered_map<std::string, std::string>
  exchange_command_line_overrides(std::unordered_map<std::string, std::string> overrides);
}  // namespace config::test_support
#endif
