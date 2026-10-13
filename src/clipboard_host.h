#pragma once

namespace clipboard_host {

  /**
   * @brief Start the host-side clipboard provider (no-op on Windows, where the
   *        GUI agent owns this role).
   * @details Installs an inbound listener on clipboard_bridge (client ->
   *          desktop clipboard) and spawns a poll thread (desktop clipboard ->
   *          clients) that runs while streaming sessions are active.
   */
  void
  start();

  /**
   * @brief Stop the provider and join its poll thread.
   */
  void
  stop();

}  // namespace clipboard_host
