#pragma once

/**
 * @file src/clipboard_host.h
 * @brief The host-side clipboard provider (desktop clipboard <-> clients).
 *
 * The provider talks to the session bus (klipper for text, the compositor's
 * data-control protocol for images), so it only exists when libsystemd is
 * available. Without it this header degrades to no-op answers instead of
 * leaving undefined symbols behind: a build without the session bus simply
 * never advertises or services clipboard frames.
 */
#if defined(SUNSHINE_CLIPBOARD_HOST)

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

#else

namespace clipboard_host {

  inline void
  start() {
  }

  inline void
  stop() {
  }

  /// No provider means no images, so the capability stays unadvertised.
  inline bool
  image_available() {
    return false;
  }

}  // namespace clipboard_host

#endif
