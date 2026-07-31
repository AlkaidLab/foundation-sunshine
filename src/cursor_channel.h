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

  bool
  local_mode_active();

  void
  set_producer_available(bool available);

  bool
  producer_available();

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
}  // namespace cursor_channel
