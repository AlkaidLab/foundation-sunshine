/**
 * @file src/subprocess/subprocess.cpp
 * @brief Implementation of subprocess module initialization.
 */
#include "subprocess.h"

#include "src/logging.h"

namespace subprocess {

  int
  init() {
    BOOST_LOG(info) << "Initializing subprocess streaming module";

    // Initialize configuration
    init_config();

    auto &cfg = get_config();
    if (cfg.enabled) {
      BOOST_LOG(info) << "Subprocess streaming mode is ENABLED";
      BOOST_LOG(info) << "  Heartbeat interval: " << cfg.heartbeat_interval_ms << "ms";
      BOOST_LOG(info) << "  Heartbeat timeout: " << cfg.heartbeat_timeout_ms << "ms";
      BOOST_LOG(info) << "  Init timeout: " << cfg.init_timeout_ms << "ms";
    }
    else {
      BOOST_LOG(info) << "Subprocess streaming mode is DISABLED (using traditional streaming)";
    }

    return 0;
  }

  void
  shutdown() {
    BOOST_LOG(info) << "Shutting down subprocess streaming module";

    // Stop all subprocess workers
    subprocess_manager_t::instance().stop_all();
  }

  bool
  is_enabled() {
    return get_config().enabled;
  }

}  // namespace subprocess
