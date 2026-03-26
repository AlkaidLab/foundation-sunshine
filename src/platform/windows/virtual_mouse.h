/**
 * @file src/platform/windows/virtual_mouse.h
 * @brief Zako Virtual Mouse client interface.
 *
 * Communicates with the UMDF virtual mouse driver via standard HID output reports.
 * When the driver is not available, provides a no-op fallback (caller should
 * use SendInput as fallback).
 */
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace platf {
  namespace vmouse {

    // Button flags (same as VMOUSE_BUTTON_* in vmouse_shared.h)
    // Named BTN_* to avoid collision with Windows BUTTON_LEFT/MIDDLE/RIGHT macros
    constexpr uint8_t BTN_LEFT = 0x01;
    constexpr uint8_t BTN_RIGHT = 0x02;
    constexpr uint8_t BTN_MIDDLE = 0x04;
    constexpr uint8_t BTN_SIDE = 0x08;   // X1 / Back
    constexpr uint8_t BTN_EXTRA = 0x10;  // X2 / Forward

    namespace detail {
      using output_report_t = std::array<uint8_t, 8>;

      output_report_t
      build_output_report(uint8_t buttons, int16_t delta_x, int16_t delta_y,
                          int8_t scroll_v, int8_t scroll_h);

      uint8_t
      apply_button_transition(uint8_t current_buttons, uint8_t button_mask,
                              bool release);

      bool
      should_close_on_write_error(unsigned long err);
    }  // namespace detail

    /**
     * @brief Virtual mouse device handle.
     *
     * Manages the HID device connection and provides methods for sending
     * mouse input through the UMDF virtual mouse driver.
     */
    class device_t {
    public:
      device_t();
      ~device_t();

      // Non-copyable
      device_t(const device_t &) = delete;
      device_t &operator=(const device_t &) = delete;

      // Movable
      device_t(device_t &&other) noexcept;
      device_t &operator=(device_t &&other) noexcept;

      /**
       * @brief Check if the virtual mouse driver is connected.
       * @return true if the HID device is open and ready.
       */
      bool
      is_available() const;

      /**
       * @brief Send a relative mouse movement.
       * @param delta_x X movement (-32767 to 32767)
       * @param delta_y Y movement (-32767 to 32767)
       * @return true on success.
       */
      bool
      move(int16_t delta_x, int16_t delta_y);

      /**
       * @brief Press or release a mouse button.
       * @param button_mask BUTTON_* flags for the button.
       * @param release true to release, false to press.
       * @return true on success.
       */
      bool
      button(uint8_t button_mask, bool release);

      /**
       * @brief Send vertical scroll.
       * @param distance Scroll distance (-127 to 127, positive = up).
       * @return true on success.
       */
      bool
      scroll(int8_t distance);

      /**
       * @brief Send horizontal scroll.
       * @param distance Scroll distance (-127 to 127, positive = right).
       * @return true on success.
       */
      bool
      hscroll(int8_t distance);

      /**
       * @brief Send a combined mouse report (movement + buttons + scroll).
       *
       * More efficient than calling move/button/scroll separately when
       * multiple changes happen simultaneously.
       *
       * @param buttons Current button state (BUTTON_* flags OR'd together)
       * @param delta_x X movement
       * @param delta_y Y movement
       * @param scroll_v Vertical scroll
       * @param scroll_h Horizontal scroll
       * @return true on success.
       */
      bool
      send_report(uint8_t buttons, int16_t delta_x, int16_t delta_y,
                  int8_t scroll_v, int8_t scroll_h);

    private:
      struct impl_t;
      std::unique_ptr<impl_t> impl;

      friend device_t create();
    };

    /**
     * @brief Try to create and connect to the virtual mouse driver.
     * @return A device_t instance. Check is_available() to see if connection succeeded.
     */
    device_t
    create();

    /**
     * @brief Check if the virtual mouse driver is installed on the system.
     * @return true if the driver device is found.
     */
    bool
    is_driver_installed();

  }  // namespace vmouse
}  // namespace platf
