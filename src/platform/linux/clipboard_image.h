/**
 * @file src/platform/linux/clipboard_image.h
 * @brief Linux image half of the host-side clipboard provider.
 *
 * The text half goes through KDE's klipper D-Bus service, which only carries
 * strings. Images therefore need the compositor's own clipboard: this module
 * speaks `ext_data_control_v1`, the privileged data-device protocol that lets a
 * client both read the current selection and publish a new one.
 *
 * It runs on its own Wayland connection and thread, so a stalled clipboard
 * source can never wedge the klipper poll loop (and vice versa). Both halves
 * feed the same clipboard_bridge / clipboard_wire wire contract, so clients see
 * one clipboard regardless of which half moved the data.
 *
 * Absent or too old compositors simply report available() == false and the
 * provider stays text-only, exactly as before.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace platf::clipboard_image {
  using png_t = std::vector<std::uint8_t>;

  /**
   * @brief Whether the running session offers `ext_data_control_manager_v1`
   *        and a seat, i.e. whether images can be synced at all.
   * @details Opens and closes a throwaway connection; call once at startup.
   */
  bool
  available();

  /**
   * @brief Owner of the image-mode Wayland connection.
   * @details Producers (desktop -> clients) deliver PNG bytes to the sink;
   *          consumers (clients -> desktop) call set_image().
   */
  class watcher_t {
  public:
    /// Receives every PNG the desktop clipboard hands us, on the watcher thread.
    using sink_fn = std::function<void(png_t)>;

    watcher_t();
    ~watcher_t();
    watcher_t(const watcher_t &) = delete;
    watcher_t &operator=(const watcher_t &) = delete;

    /**
     * @brief Connect and start watching the selection.
     * @return false when the session lacks the protocol or the connection
     *         fails; the watcher then stays inert.
     */
    bool
    start(sink_fn sink);

    /// Drop the connection, releasing the selection we own.
    void
    stop();

    /**
     * @brief Queue a PNG to become the desktop clipboard.
     * @details Asynchronous and newest-wins: the queue holds at most one image,
     *          because a stale clipboard is useless once a newer one arrives.
     * @return false when the watcher is not running.
     */
    bool
    set_image(png_t png);

  private:
    struct impl_t;
    std::unique_ptr<impl_t> _impl;
  };
}  // namespace platf::clipboard_image
