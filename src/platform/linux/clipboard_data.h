/**
 * @file src/platform/linux/clipboard_data.h
 * @brief Linux half of the host-side clipboard provider, over `ext_data_control_v1`.
 *
 * KDE's klipper D-Bus service carries strings and nothing else, so it can never be
 * the whole provider: images need the compositor's own clipboard, and on any desktop
 * without klipper (niri, sway, Hyprland) text needs it too. This module speaks
 * `ext_data_control_v1`, the privileged data-device protocol that lets a client both
 * read the current selection and publish a new one, and carries both flavours.
 *
 * It runs on its own Wayland connection and thread, so a stalled clipboard source can
 * never wedge the klipper poll loop (and vice versa). Everything feeds the same
 * clipboard_bridge / clipboard_wire contract, so clients see one clipboard regardless
 * of which transport moved the data.
 *
 * Absent or too old compositors simply report available() == false and the provider
 * stays on klipper alone, exactly as before.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace platf::clipboard_data {
  using bytes_t = std::vector<std::uint8_t>;
  using png_t = bytes_t;

  /**
   * @brief Whether the running session offers `ext_data_control_manager_v1`
   *        and a seat, i.e. whether this transport can move anything at all.
   * @details Opens and closes a throwaway connection; call once at startup.
   */
  bool
  available();

  /**
   * @brief Owner of the data-control Wayland connection.
   * @details Producers (desktop -> clients) deliver bytes to the sinks; consumers
   *          (clients -> desktop) call set_image() / set_text().
   */
  class watcher_t {
  public:
    /// Receives every PNG the desktop clipboard hands us, on the watcher thread.
    using sink_fn = std::function<void(png_t)>;
    /// Receives every text item the desktop clipboard hands us, on the watcher thread.
    using text_sink_fn = std::function<void(std::string)>;

    watcher_t();
    ~watcher_t();
    watcher_t(const watcher_t &) = delete;
    watcher_t &operator=(const watcher_t &) = delete;

    /**
     * @brief Connect and start watching the selection.
     * @param sink Receives PNG items; may be empty to ignore images.
     * @param text_sink Receives text items; may be empty to ignore text.
     * @return false when the session lacks the protocol or the connection
     *         fails; the watcher then stays inert.
     */
    bool
    start(sink_fn sink, text_sink_fn text_sink = {});

    /// Drop the connection, releasing the selection we own.
    void
    stop();

    /**
     * @brief Queue a PNG to become the desktop clipboard.
     * @details Asynchronous and newest-wins: the queue holds at most one item,
     *          because a stale clipboard is useless once a newer one arrives.
     * @return false when the watcher is not running.
     */
    bool
    set_image(png_t png);

    /**
     * @brief Queue text to become the desktop clipboard.
     * @details Shares the queue with set_image(), so text and images cannot both
     *          be pending; the newest write wins.
     */
    bool
    set_text(std::string text);

  private:
    struct impl_t;
    std::unique_ptr<impl_t> _impl;
  };
}  // namespace platf::clipboard_data
