/**
 * @file tests/unit/test_ds5_button_map.cpp
 * @brief Moonlight buttons and sticks to the DualSense input report, checked on the report bytes.
 */
#include "src/ds5/button_map.h"

#include "alkaidlab/ds5_core/ds5_core.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace {
  namespace bm = ds5::button_map;
  namespace fl = ds5::button_map::flag;

  using Report = std::array<std::uint8_t, ALK_DS5_USB_INPUT_REPORT_BYTES>;

  Report
  build(std::uint32_t flags, std::uint8_t lt = 0, std::uint8_t rt = 0, std::int16_t lx = 0, std::int16_t ly = 0,
    std::int16_t rx = 0, std::int16_t ry = 0) {
    AlkDs5State state { sizeof(state), ALK_DS5_CORE_ABI_VERSION, 0, 1, 0, bm::hat(flags), lt, rt, 0,
      bm::stick_x(lx), bm::stick_y(ly), bm::stick_x(rx), bm::stick_y(ry), 0 };
    state.buttons = bm::buttons(flags, lt, rt);
    Report report {};
    std::size_t length = 0;
    EXPECT_EQ(alk_ds5_build_input_report(&state, report.data(), report.size(), &length), ALK_DS5_OK);
    return report;
  }

  // Byte 8: hat in the low nibble, square 0x10, cross 0x20, circle 0x40, triangle 0x80.
  // Byte 9: L1 0x01, R1 0x02, L2 0x04, R2 0x08, create 0x10, options 0x20, L3 0x40, R3 0x80.
  // Byte 10: PS 0x01, touchpad click 0x02, microphone mute 0x04.
  // Source: Linux hid-playstation DS_BUTTONS0/1/2 (ds5-linux-hid-playstation-20261031).
  constexpr std::size_t byte0 = 8, byte1 = 9, byte2 = 10;

  TEST(Ds5ButtonMap, FaceButtonsFollowThePlayStationLayout) {
    // Xbox-style A/B/X/Y become cross/circle/square/triangle.
    EXPECT_EQ(build(fl::a)[byte0], 0x28);  // cross, hat released
    EXPECT_EQ(build(fl::b)[byte0], 0x48);  // circle
    EXPECT_EQ(build(fl::x)[byte0], 0x18);  // square
    EXPECT_EQ(build(fl::y)[byte0], 0x88);  // triangle
  }

  TEST(Ds5ButtonMap, ShouldersStickClicksAndSystemButtons) {
    EXPECT_EQ(build(fl::left_shoulder)[byte1], 0x01);
    EXPECT_EQ(build(fl::right_shoulder)[byte1], 0x02);
    EXPECT_EQ(build(fl::back)[byte1], 0x10);  // create
    EXPECT_EQ(build(fl::start)[byte1], 0x20);  // options
    EXPECT_EQ(build(fl::left_stick)[byte1], 0x40);  // L3
    EXPECT_EQ(build(fl::right_stick)[byte1], 0x80);  // R3
    EXPECT_EQ(build(fl::home)[byte2], 0x01);  // PS
    EXPECT_EQ(build(fl::touchpad)[byte2], 0x02);
    EXPECT_EQ(build(fl::misc)[byte2], 0x04);  // microphone mute
  }

  TEST(Ds5ButtonMap, NoButtonLeavesOnlyTheReleasedHat) {
    const auto report = build(0);
    EXPECT_EQ(report[byte0], 0x08);
    EXPECT_EQ(report[byte1], 0);
    EXPECT_EQ(report[byte2], 0);
  }

  TEST(Ds5ButtonMap, TriggersSetTheDigitalBitFromTheAnalogValue) {
    EXPECT_EQ(build(0, 0, 0)[byte1], 0);
    EXPECT_EQ(build(0, bm::trigger_digital_threshold - 1, 0)[byte1], 0);
    EXPECT_EQ(build(0, bm::trigger_digital_threshold, 0)[byte1], 0x04);
    EXPECT_EQ(build(0, 0, 255)[byte1], 0x08);
    const auto both = build(0, 255, 255);
    EXPECT_EQ(both[byte1], 0x0c);
    EXPECT_EQ(both[5], 255);  // the analog values pass through
    EXPECT_EQ(both[6], 255);
  }

  TEST(Ds5ButtonMap, HatCoversEightDirectionsAndCancelsOpposites) {
    EXPECT_EQ(bm::hat(0), 8);
    EXPECT_EQ(bm::hat(fl::dpad_up), 0);
    EXPECT_EQ(bm::hat(fl::dpad_up | fl::dpad_right), 1);
    EXPECT_EQ(bm::hat(fl::dpad_right), 2);
    EXPECT_EQ(bm::hat(fl::dpad_right | fl::dpad_down), 3);
    EXPECT_EQ(bm::hat(fl::dpad_down), 4);
    EXPECT_EQ(bm::hat(fl::dpad_down | fl::dpad_left), 5);
    EXPECT_EQ(bm::hat(fl::dpad_left), 6);
    EXPECT_EQ(bm::hat(fl::dpad_left | fl::dpad_up), 7);
    EXPECT_EQ(bm::hat(fl::dpad_up | fl::dpad_down), 8);
    EXPECT_EQ(bm::hat(fl::dpad_left | fl::dpad_right), 8);
    EXPECT_EQ(bm::hat(fl::dpad_up | fl::dpad_down | fl::dpad_left), 6);
    EXPECT_EQ(build(fl::dpad_right)[byte0] & 0x0f, 2);
  }

  TEST(Ds5ButtonMap, EveryButtonSetsExactlyOneBit) {
    for (const auto flag : { fl::a, fl::b, fl::x, fl::y, fl::left_shoulder, fl::right_shoulder, fl::back, fl::start,
           fl::left_stick, fl::right_stick, fl::home, fl::touchpad, fl::misc }) {
      const auto report = build(flag);
      const auto bits = __builtin_popcount(report[byte0] & 0xf0) + __builtin_popcount(report[byte1]) +
                        __builtin_popcount(report[byte2]);
      EXPECT_EQ(bits, 1) << flag;
    }
    // All at once leaves no bit shared between two buttons.
    const auto all = build(fl::a | fl::b | fl::x | fl::y | fl::left_shoulder | fl::right_shoulder | fl::back |
                           fl::start | fl::left_stick | fl::right_stick | fl::home | fl::touchpad | fl::misc);
    EXPECT_EQ(all[byte0] & 0xf0, 0xf0);
    EXPECT_EQ(all[byte1], 0xf3);
    EXPECT_EQ(all[byte2], 0x07);
  }

  TEST(Ds5ButtonMap, SticksCoverTheFullRangeAndYIsInverted) {
    // Report offsets: left X 1, left Y 2, right X 3, right Y 4.
    const auto centre = build(0);
    for (const auto offset : { 1, 2, 3, 4 }) {
      EXPECT_EQ(centre[offset], 128) << offset;  // a centred stick reads 0x80
    }
    EXPECT_EQ(bm::stick_x(-32768), 0);
    EXPECT_EQ(bm::stick_x(32767), 255);
    EXPECT_EQ(bm::stick_y(32767), 0) << "pushing up must read 0, the top of the report range";
    EXPECT_EQ(bm::stick_y(-32768), 255);
    EXPECT_EQ(bm::stick_y(0), 128);
    // Monotonic over the whole range, with no wrap at either end.
    int previous_x = -1, previous_y = 256;
    for (int value = -32768; value <= 32767; value += 7) {
      const int x = bm::stick_x(static_cast<std::int16_t>(value));
      const int y = bm::stick_y(static_cast<std::int16_t>(value));
      EXPECT_GE(x, previous_x);
      EXPECT_LE(y, previous_y);
      previous_x = x;
      previous_y = y;
    }
    const auto up_left = build(0, 0, 0, -32768, 32767, 32767, -32768);
    EXPECT_EQ(up_left[1], 0);
    EXPECT_EQ(up_left[2], 0);
    EXPECT_EQ(up_left[3], 255);
    EXPECT_EQ(up_left[4], 255);
  }
}  // namespace
