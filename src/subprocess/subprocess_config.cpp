/**
 * @file src/subprocess/subprocess_config.cpp
 * @brief Implementation of subprocess configuration.
 */
#include "subprocess_config.h"

#include "src/config.h"
#include "src/logging.h"

namespace subprocess {

  static config_t g_config;

  config_t &
  get_config() {
    return g_config;
  }

  void
  init_config() {
    // Initialize from main config
    // For now, subprocess mode is disabled by default
    // Users can enable it via configuration
    g_config.enabled = false;

    // Set default values
    g_config.heartbeat_interval_ms = 1000;
    g_config.heartbeat_timeout_ms = 5000;
    g_config.init_timeout_ms = 10000;

    // Auto-detect sender executable path based on platform
#ifdef _WIN32
    g_config.sender_executable = "";  // Will be set to sunshine-sender.exe in same directory
#else
    g_config.sender_executable = "";  // Will be set to sunshine-sender in same directory
#endif

    BOOST_LOG(debug) << "Subprocess streaming mode: "
                     << (g_config.enabled ? "enabled" : "disabled");
  }

}  // namespace subprocess
