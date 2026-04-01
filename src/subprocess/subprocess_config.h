/**
 * @file src/subprocess/subprocess_config.h
 * @brief Configuration for subprocess streaming mode.
 */
#pragma once

#include <string>

namespace subprocess {

  /**
   * @brief Subprocess mode configuration.
   */
  struct config_t {
    bool enabled = false;                    ///< Enable subprocess streaming mode
    std::string sender_executable;           ///< Path to sender executable (auto-detect if empty)
    int heartbeat_interval_ms = 1000;        ///< Heartbeat interval in milliseconds
    int heartbeat_timeout_ms = 5000;         ///< Heartbeat timeout in milliseconds
    int init_timeout_ms = 10000;             ///< Subprocess initialization timeout
  };

  /**
   * @brief Get the global subprocess configuration.
   */
  config_t &
  get_config();

  /**
   * @brief Initialize subprocess configuration from main config.
   */
  void
  init_config();

}  // namespace subprocess
