/**
 * @file src/platform/windows/display_cursor.h
 * @brief CPU cursor conversion and local-cursor publication helpers.
 */
#pragma once

#include "display.h"

namespace platf::dxgi {
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

  /**
   * Publish visibility and, when present, a canonical BGRA cursor shape.
   *
   * @return true when the current shape was consumed and may be acknowledged.
   */
  bool
  publish_local_cursor(const vdd_capture_t::cursor_snapshot &cursor);
}  // namespace platf::dxgi
