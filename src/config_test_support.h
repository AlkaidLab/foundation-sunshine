/**
 * @file src/config_test_support.h
 * @brief Test-only controls for configuration-save contention.
 */
#pragma once

#ifdef SUNSHINE_TESTS
  #include <functional>

namespace config::test_support {
  /** Observe an update only after its attempt to acquire the save mutex failed. */
  void
  set_save_lock_contended_hook(std::function<void()> hook);

}  // namespace config::test_support
#endif
