/**
 * @file src/subprocess/subprocess.h
 * @brief Main include file for subprocess streaming module.
 *
 * This module implements a separated architecture where:
 * - Main process (SYSTEM - Control Plane): Handles RTSP handshake, authentication, control commands
 * - Subprocess (User - Data Plane): Handles capture, encoding, sending video/audio data
 *
 * Benefits:
 * - Lower latency by avoiding cross-process memory copies
 * - Better WGC/audio compatibility (user session access)
 * - Simpler architecture with clear separation of concerns
 */
#pragma once

#include "ipc_pipe.h"
#include "ipc_protocol.h"
#include "subprocess_config.h"
#include "subprocess_manager.h"

namespace subprocess {

  /**
   * @brief Initialize the subprocess module.
   * @return 0 on success, non-zero on failure.
   */
  int
  init();

  /**
   * @brief Shutdown the subprocess module.
   */
  void
  shutdown();

  /**
   * @brief Check if subprocess streaming mode is enabled.
   * @return true if enabled.
   */
  bool
  is_enabled();

}  // namespace subprocess
