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

  /**
   * @brief Whether the image half is up, i.e. whether this session can apply
   *        and publish clipboard images.
   * @details Always false off Wayland: images ride the compositor's
   *          data-control protocol, which the klipper text path cannot reach.
   *          Used to decide whether the image capability is advertised, so a
   *          client never sends frames nothing can service.
   */
  bool
  image_available();

}  // namespace clipboard_host
