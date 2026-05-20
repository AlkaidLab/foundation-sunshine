/**
 * @file src/platform/windows/input_hid.h
 * @brief Zako Input HID Bus client interface.
 */
#pragma once

#include <cstdint>
#include <memory>

namespace platf {
  namespace input_hid {

    constexpr uint8_t BTN_LEFT = 0x01;
    constexpr uint8_t BTN_RIGHT = 0x02;
    constexpr uint8_t BTN_MIDDLE = 0x04;
    constexpr uint8_t BTN_SIDE = 0x08;
    constexpr uint8_t BTN_EXTRA = 0x10;

    class device_t {
    public:
      device_t();
      ~device_t();

      device_t(const device_t &) = delete;
      device_t &operator=(const device_t &) = delete;
      device_t(device_t &&other) noexcept;
      device_t &operator=(device_t &&other) noexcept;

      bool
      is_available() const;

      bool
      move(int16_t delta_x, int16_t delta_y);

      bool
      button(uint8_t button_mask, bool release);

      bool
      scroll(int8_t distance);

      bool
      hscroll(int8_t distance);

      bool
      keyboard_update(uint16_t vk, bool release, uint8_t flags);

      bool
      release_all();

    private:
      struct impl_t;
      std::unique_ptr<impl_t> impl;

      friend device_t create();
    };

    device_t
    create();

  }  // namespace input_hid
}  // namespace platf
