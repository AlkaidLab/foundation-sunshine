/**
 * @file src/input.h
 * @brief Declarations for gamepad, keyboard, and mouse input handling.
 */
#pragma once

#include <cstdint>
#include <functional>

#include "platform/common.h"
#include "thread_safe.h"

namespace input {
  struct input_t;

  void
  print(void *input);
  void
  reset(std::shared_ptr<input_t> &input);
  void
  passthrough(std::shared_ptr<input_t> &input, std::vector<std::uint8_t> &&input_data);

  struct diagnostics_snapshot_t {
    std::uint32_t input_queue_depth {};
    std::uint32_t release_queue_delay_us {};
    std::uint32_t coalesced_pointer_deltas {};
    std::uint32_t pointer_acceleration_risk_ppm {};
    bool release_smoothing_active {};
  };

  [[nodiscard]] diagnostics_snapshot_t
  diagnostics_snapshot(const std::shared_ptr<input_t> &input);

  [[nodiscard]] std::unique_ptr<platf::deinit_t>
  init();

  bool
  probe_gamepads();

  std::shared_ptr<input_t>
  alloc(safe::mail_t mail);

  struct touch_port_t: public platf::touch_port_t {
    int env_width, env_height;

    // Offset x and y coordinates of the client
    float client_offsetX, client_offsetY;

    float scalar_inv;

    explicit
    operator bool() const {
      return width != 0 && height != 0 && env_width != 0 && env_height != 0;
    }
  };

  /**
   * @brief Scale the ellipse axes according to the provided size.
   * @param val The major and minor axis pair.
   * @param rotation The rotation value from the touch/pen event.
   * @param scalar The scalar cartesian coordinate pair.
   * @return The major and minor axis pair.
   */
  std::pair<float, float>
  scale_client_contact_area(const std::pair<float, float> &val, uint16_t rotation, const std::pair<float, float> &scalar);
}  // namespace input
