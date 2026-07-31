/**
 * @file src/platform/windows/display_cursor.h
 * @brief CPU cursor conversion and local-cursor publication helpers.
 */
#pragma once

#include "display.h"

namespace platf::dxgi {
  struct normalized_cursor_shape_t {
    DXGI_OUTDUPL_POINTER_SHAPE_INFO info {};
    util::buffer_t<std::uint8_t> alpha;
    util::buffer_t<std::uint8_t> xor_mask;
  };

  util::buffer_t<std::uint8_t>
  make_cursor_xor_image(const util::buffer_t<std::uint8_t> &img_data,
                        DXGI_OUTDUPL_POINTER_SHAPE_INFO shape_info);

  util::buffer_t<std::uint8_t>
  make_cursor_alpha_image(const util::buffer_t<std::uint8_t> &img_data,
                          DXGI_OUTDUPL_POINTER_SHAPE_INFO shape_info);

  bool
  set_cursor_texture(device_t::pointer device,
                     gpu_cursor_t &cursor,
                     util::buffer_t<std::uint8_t> &&cursor_img,
                     DXGI_OUTDUPL_POINTER_SHAPE_INFO &shape_info);

  bool
  local_cursor_mode_active();

  bool
  sync_local_cursor_mode(duplication_t &duplication);

  /**
   * Convert a captured cursor shape into tightly packed DXGI cursor images.
   * The XOR image is omitted when include_xor is false.
   */
  bool
  normalize_cursor_shape(const std::vector<std::uint8_t> &shape_buffer,
                         DXGI_OUTDUPL_POINTER_SHAPE_INFO shape_info,
                         bool include_xor,
                         normalized_cursor_shape_t &normalized);

  bool
  normalize_cursor_shape(const vdd_capture_t::cursor_snapshot &cursor,
                         bool include_xor,
                         normalized_cursor_shape_t &normalized);

  /**
   * Publish visibility and, when present, a canonical BGRA cursor shape.
   *
   * @return true when a shape was published or an empty hidden shape was
   * handled; false when this update had no shape or its shape was rejected.
   */
  bool
  publish_local_cursor(const vdd_capture_t::cursor_snapshot &cursor);

  bool
  publish_local_cursor(const cursor_t &cursor, bool shape_updated);
}  // namespace platf::dxgi
