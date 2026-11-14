/**
 * @file src/ds5/button_map.h
 * @brief Moonlight button flags to the DualSense input report button bits.
 *
 * The report layout is the one of the Linux hid-playstation driver (DS_BUTTONS0/1/2): byte 8 holds
 * the hat in its low nibble and square, cross, circle, triangle in bits 4 to 7; byte 9 holds L1, R1,
 * L2, R2, create, options, L3, R3; byte 10 holds PS, touchpad click and microphone mute. The core's
 * button word starts at byte 8 bit 4, so bit 0 is square and bit 12 is PS.
 */
#pragma once

#include <cstdint>

namespace ds5::button_map {
  /** @brief Moonlight/Sunshine button flags (Limelight.h), the values of platf::*. */
  namespace flag {
    constexpr std::uint32_t dpad_up = 0x0001;
    constexpr std::uint32_t dpad_down = 0x0002;
    constexpr std::uint32_t dpad_left = 0x0004;
    constexpr std::uint32_t dpad_right = 0x0008;
    constexpr std::uint32_t start = 0x0010;
    constexpr std::uint32_t back = 0x0020;
    constexpr std::uint32_t left_stick = 0x0040;
    constexpr std::uint32_t right_stick = 0x0080;
    constexpr std::uint32_t left_shoulder = 0x0100;
    constexpr std::uint32_t right_shoulder = 0x0200;
    constexpr std::uint32_t home = 0x0400;
    constexpr std::uint32_t a = 0x1000;
    constexpr std::uint32_t b = 0x2000;
    constexpr std::uint32_t x = 0x4000;
    constexpr std::uint32_t y = 0x8000;
    constexpr std::uint32_t touchpad = 0x100000;
    constexpr std::uint32_t misc = 0x200000;
  }  // namespace flag

  /** @brief Bit positions in the core's button word. */
  namespace bit {
    constexpr std::uint64_t square = 1ull << 0;
    constexpr std::uint64_t cross = 1ull << 1;
    constexpr std::uint64_t circle = 1ull << 2;
    constexpr std::uint64_t triangle = 1ull << 3;
    constexpr std::uint64_t l1 = 1ull << 4;
    constexpr std::uint64_t r1 = 1ull << 5;
    constexpr std::uint64_t l2 = 1ull << 6;
    constexpr std::uint64_t r2 = 1ull << 7;
    constexpr std::uint64_t create = 1ull << 8;
    constexpr std::uint64_t options = 1ull << 9;
    constexpr std::uint64_t l3 = 1ull << 10;
    constexpr std::uint64_t r3 = 1ull << 11;
    constexpr std::uint64_t ps = 1ull << 12;
    constexpr std::uint64_t touchpad_click = 1ull << 13;
    constexpr std::uint64_t mic_mute = 1ull << 14;
  }  // namespace bit

  /**
   * @brief The analog trigger value from which the digital L2/R2 button reads as pressed.
   *
   * The device sets its own digital bit and neither the Linux hid-playstation driver (BTN_TL2/TR2 from
   * DS_BUTTONS1_L2/R2) nor SDL's PS5 driver documents the analog value at which it flips; SDL only
   * treats a set bit with an analog value of zero as a full press. The protocol carries the analog
   * value but no digital bit, so the bit is derived. It is never set at an analog value of zero (so
   * SDL-style consumers always use the analog value), and the exact threshold is not verified on a
   * pad.
   */
  constexpr std::uint8_t trigger_digital_threshold = 0x20;

  /**
   * @brief Hat switch value: 0 up, 1 up-right, 2 right, 3 down-right, 4 down, 5 down-left, 6 left,
   *        7 up-left, 8 released. Opposite directions cancel.
   */
  constexpr std::uint8_t
  hat(std::uint32_t flags) {
    const int vertical = ((flags & flag::dpad_down) ? 1 : 0) - ((flags & flag::dpad_up) ? 1 : 0);
    const int horizontal = ((flags & flag::dpad_right) ? 1 : 0) - ((flags & flag::dpad_left) ? 1 : 0);
    constexpr std::uint8_t table[3][3] = {
      // horizontal: left, none, right
      { 7, 0, 1 },  // up
      { 6, 8, 2 },  // none
      { 5, 4, 3 },  // down
    };
    return table[vertical + 1][horizontal + 1];
  }

  /** @brief Stick X: 0 left, 128 centre, 255 right (rounded, so a centred stick reads 128). */
  constexpr std::uint8_t
  stick_x(std::int16_t value) {
    const std::int32_t scaled = (static_cast<std::int32_t>(value) + 32768 + 128) / 257;
    return static_cast<std::uint8_t>(scaled > 255 ? 255 : scaled);
  }

  /** @brief Stick Y: Moonlight sends up as positive, the DualSense report has 0 at the top. */
  constexpr std::uint8_t
  stick_y(std::int16_t value) {
    const std::int32_t scaled = (32768 - static_cast<std::int32_t>(value) + 128) / 257;
    return static_cast<std::uint8_t>(scaled > 255 ? 255 : scaled);
  }

  /** @brief The core button word for Moonlight button flags and the analog trigger values. */
  constexpr std::uint64_t
  buttons(std::uint32_t flags, std::uint8_t left_trigger, std::uint8_t right_trigger) {
    std::uint64_t word = 0;
    if (flags & flag::x) word |= bit::square;
    if (flags & flag::a) word |= bit::cross;
    if (flags & flag::b) word |= bit::circle;
    if (flags & flag::y) word |= bit::triangle;
    if (flags & flag::left_shoulder) word |= bit::l1;
    if (flags & flag::right_shoulder) word |= bit::r1;
    if (left_trigger >= trigger_digital_threshold) word |= bit::l2;
    if (right_trigger >= trigger_digital_threshold) word |= bit::r2;
    if (flags & flag::back) word |= bit::create;
    if (flags & flag::start) word |= bit::options;
    if (flags & flag::left_stick) word |= bit::l3;
    if (flags & flag::right_stick) word |= bit::r3;
    if (flags & flag::home) word |= bit::ps;
    if (flags & flag::touchpad) word |= bit::touchpad_click;
    if (flags & flag::misc) word |= bit::mic_mute;
    return word;
  }
}  // namespace ds5::button_map
