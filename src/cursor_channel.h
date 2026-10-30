/**
 * @file src/cursor_channel.h
 * @brief Latest-value bridge between display capture and the control stream.
 */
#pragma once

#include <cstdint>
#include <vector>

namespace cursor_channel {
  struct snapshot_t {
    std::uint64_t revision = 0;
    bool valid = false;
    bool visible = false;
    bool has_shape = false;
    std::uint32_t shape_id = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::int16_t hotspot_x = 0;
    std::int16_t hotspot_y = 0;
    std::vector<std::uint8_t> bgra;
  };

  void
  set_session_enabled(std::uint32_t session_id, bool enabled);

  void
  remove_session(std::uint32_t session_id);

  /**
   * @brief Whether a cursor protocol v1 session is in local cursor mode.
   */
  bool
  local_mode_active();

  /**
   * @brief Register or release a cursor protocol v2 consumer of capture shapes (the cursor tracker).
   */
  void
  add_shape_consumer();

  void
  remove_shape_consumer();

  /**
   * @brief Whether capture should publish cursor shapes: a v1 local session or a v2 consumer exists.
   */
  bool
  shapes_wanted();

  /**
   * @brief Changes whenever capture must publish its current shape again, because
   *        the first consumer arrived after the last shape was reported.
   */
  std::uint64_t
  publish_epoch();

  /**
   * @brief Cursor protocol v2: record whether every connected session draws the cursor itself.
   */
  void
  set_all_sessions_draw(bool all_draw);

  bool
  all_sessions_draw();

  /**
   * @brief Whether frames leave the cursor out because clients draw it: a v1 local
   *        session exists (v1 rule), or every session draws it (v2 rule).
   */
  bool
  frames_exclude_cursor();

  void
  set_producer_available(bool available);

  bool
  producer_available();

  /**
   * @brief Whether the active capture backend draws the cursor into frames when asked to.
   */
  void
  set_capture_composites_cursor(bool composites);

  bool
  capture_composites_cursor();

  void
  publish_visibility(bool visible, std::uint32_t shape_id);

  void
  publish_shape(bool visible,
                std::uint32_t shape_id,
                std::uint16_t width,
                std::uint16_t height,
                std::int16_t hotspot_x,
                std::int16_t hotspot_y,
                std::vector<std::uint8_t> bgra);

  bool
  copy_latest(std::uint64_t known_revision, snapshot_t &out);

  /**
   * @brief Copy the newest published shape if it is newer than `known_shape_revision`.
   *
   * Unlike copy_latest(), visibility changes do not count, so a consumer that only
   * needs pixels does not copy them on every frame.
   */
  bool
  copy_shape_if_newer(std::uint64_t &known_shape_revision, snapshot_t &out);
}  // namespace cursor_channel
