#pragma once

#include <string>

namespace platf::vulkan_hdr_bridge {

  struct status_t {
    std::string state;
    std::string message;
    // True while the layer is active or its cleanup still needs to be retried.
    bool registered;
    bool artifacts_installed;
  };

  // Remove a registration left by a terminated Sunshine process.
  void
  startup_cleanup();

  // Validate the current interactive user's Vulkan ICD, then register the
  // packaged layer for the active HDR Zako display session.
  bool
  enable_for_vdd_hdr_session();

  // Run the same probe and per-user registration path on demand, then clean
  // it up immediately. Used by WebUI diagnostics while no stream is active.
  bool
  validate_now();

  // Normal session cleanup with diagnostics.
  void
  disable();

  // Final process teardown; avoids logging after the logging subsystem dies.
  void
  shutdown_cleanup();

  // Thread-safe snapshot for the authenticated WebUI diagnostics endpoint.
  status_t
  status();

}  // namespace platf::vulkan_hdr_bridge
