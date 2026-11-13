/**
 * @file src/video_touch_port.h
 * @brief Pure geometry helpers for the client touch/pen input port.
 */
#pragma once

#include <cmath>

#include "input.h"

namespace video {
  /**
   * @brief Build a touch/pen input port for one captured target rectangle.
   *
   * The returned port maps the negotiated client viewport (`client_width` x
   * `client_height`) to a target rectangle in virtual-screen coordinates
   * (`target_*`) while preserving the client's aspect ratio and recording the
   * resulting letterbox offsets / inverse scale.
   *
   * This is deliberately a header-only pure function so the geometry can be
   * unit-tested without a display backend.
   */
  inline input::touch_port_t
  make_touch_port_for_rect(
    int client_width,
    int client_height,
    int target_width,
    int target_height,
    int target_offset_x,
    int target_offset_y,
    int env_width,
    int env_height) {
    const float wd = static_cast<float>(target_width);
    const float hd = static_cast<float>(target_height);

    const float wt = static_cast<float>(client_width);
    const float ht = static_cast<float>(client_height);

    const auto scalar = std::fminf(wt / wd, ht / hd);

    const auto w2 = scalar * wd;
    const auto h2 = scalar * hd;

    const auto offsetX = (static_cast<float>(client_width) - w2) * 0.5f;
    const auto offsetY = (static_cast<float>(client_height) - h2) * 0.5f;

    return input::touch_port_t {
      {
        target_offset_x,
        target_offset_y,
        client_width,
        client_height,
      },
      env_width,
      env_height,
      target_width,
      target_height,
      offsetX,
      offsetY,
      1.0f / scalar,
    };
  }

  inline bool
  touch_ports_equal(const input::touch_port_t &lhs, const input::touch_port_t &rhs) {
    return lhs.offset_x == rhs.offset_x &&
           lhs.offset_y == rhs.offset_y &&
           lhs.width == rhs.width &&
           lhs.height == rhs.height &&
           lhs.env_width == rhs.env_width &&
           lhs.env_height == rhs.env_height &&
           lhs.display_width == rhs.display_width &&
           lhs.display_height == rhs.display_height &&
           lhs.client_offsetX == rhs.client_offsetX &&
           lhs.client_offsetY == rhs.client_offsetY &&
           lhs.scalar_inv == rhs.scalar_inv;
  }
}  // namespace video
