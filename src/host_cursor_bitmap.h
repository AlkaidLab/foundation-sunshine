/**
 * @file src/host_cursor_bitmap.h
 * @brief Converts Windows cursor bitmaps into the straight-alpha BGRA shapes cursor protocol v2 sends.
 *
 * Portable so the conversion rules are unit tested on every platform. Shapes that
 * Desktop Duplication or the VDD driver report are converted by
 * platf::dxgi::make_local_cursor_image(); this file handles cursors read with
 * GetIconInfo(), and limits every shape to the wire's 256x256.
 */
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace host_cursor_bitmap {
  struct image_t {
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint16_t hotspot_x = 0;
    std::uint16_t hotspot_y = 0;
    double pixel_scale = 1;  // display pixels per shape pixel (> 1 after downscaling a huge cursor)
    std::vector<std::uint8_t> bgra;  // width * height * 4, straight alpha, top-down

    bool
    operator==(const image_t &) const = default;
  };

  // A pixel that inverts the screen cannot be drawn by a client cursor. As in
  // platf::dxgi::make_local_cursor_image(), it becomes opaque white and any
  // transparent pixel next to it opaque black, so an inverting I-beam stays
  // visible on both light and dark content.

  /**
   * @brief Convert a monochrome cursor (a GDI cursor without a colour bitmap).
   *
   * `masks` holds the 1 bpp AND mask rows followed by the XOR mask rows, `pitch`
   * bytes each, most significant bit first; `height` is the visible height.
   * AND/XOR: 0/0 black, 0/1 white, 1/0 transparent, 1/1 invert.
   */
  std::optional<image_t>
  from_monochrome(std::uint32_t width,
                  std::uint32_t height,
                  std::uint32_t pitch,
                  std::span<const std::uint8_t> masks,
                  std::uint32_t hotspot_x,
                  std::uint32_t hotspot_y);

  /**
   * @brief Convert 32 bpp BGRA with straight alpha (a GDI colour cursor with alpha,
   *        or a shape already converted for the client).
   */
  std::optional<image_t>
  from_color(std::uint32_t width,
             std::uint32_t height,
             std::uint32_t pitch,
             std::span<const std::uint8_t> bgra,
             std::uint32_t hotspot_x,
             std::uint32_t hotspot_y);

  /**
   * @brief Convert a GDI colour cursor without alpha, where a 1 bpp AND mask selects
   *        XOR (1) or replace (0).
   */
  std::optional<image_t>
  from_color_and_mask(std::uint32_t width,
                      std::uint32_t height,
                      std::uint32_t color_pitch,
                      std::span<const std::uint8_t> bgra,
                      std::uint32_t mask_pitch,
                      std::span<const std::uint8_t> and_mask,
                      std::uint32_t hotspot_x,
                      std::uint32_t hotspot_y);

  /**
   * @brief Display pixels per pixel of a cursor bitmap read with GetIconInfo().
   *
   * Windows draws a cursor of the standard size at the standard size for the DPI
   * of the display it is on, but GetIconInfo() returns it at the standard size for
   * the reading process's system DPI. The standard sizes are SM_CXCURSOR at each
   * DPI. A cursor of any other size is drawn as it is.
   */
  double
  gdi_display_scale(std::uint32_t width, std::uint32_t height, std::uint32_t read_standard, std::uint32_t display_standard);

  /**
   * @brief 64-bit FNV-1a over dimensions, hotspot, scale and pixels.
   */
  std::uint64_t
  content_hash(const image_t &image);
}  // namespace host_cursor_bitmap
