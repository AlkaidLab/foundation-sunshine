/**
 * @file src/config_test_support.h
 * @brief Test-only controls for startup command-line policy.
 */
#pragma once

#ifdef SUNSHINE_TESTS
  #include <string>
  #include <unordered_map>

namespace config::test_support {
  /** Replace startup overrides in isolated tests and return the previous policy for restoration. */
  std::unordered_map<std::string, std::string>
  exchange_command_line_overrides(std::unordered_map<std::string, std::string> overrides);
}  // namespace config::test_support
#endif
