/**
 * @file src/confighttp.h
 * @brief Public Web UI server entry point.
 */
#pragma once

// Also used by the tray icon asset paths.
#define WEB_DIR SUNSHINE_ASSETS_DIR "/web/"

namespace confighttp {
  constexpr auto PORT_HTTPS = 1;
  void
  start();
}  // namespace confighttp
