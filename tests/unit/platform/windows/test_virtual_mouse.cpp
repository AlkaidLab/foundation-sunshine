/**
 * @file tests/unit/platform/windows/test_virtual_mouse.cpp
 * @brief Test virtual mouse HID report helpers.
 */

#ifdef _WIN32

  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>

  #include <src/platform/windows/virtual_mouse.h>

  #include "../../../tests_common.h"

TEST(VMouseReportTests, BuildOutputReportSerializesLittleEndianSignedDeltas) {
  const auto report = platf::vmouse::detail::build_output_report(
    platf::vmouse::BTN_LEFT | platf::vmouse::BTN_SIDE,
    0x1234,
    static_cast<int16_t>(-200),
    12,
    static_cast<int8_t>(-7));

  const platf::vmouse::detail::output_report_t expected {
    0x02,
    static_cast<uint8_t>(platf::vmouse::BTN_LEFT | platf::vmouse::BTN_SIDE),
    0x34,
    0x12,
    0x38,
    0xFF,
    0x0C,
    0xF9,
  };

  EXPECT_EQ(report, expected);
}

TEST(VMouseReportTests, ApplyButtonTransitionMaintainsAggregateState) {
  uint8_t buttons = 0;

  buttons = platf::vmouse::detail::apply_button_transition(buttons, platf::vmouse::BTN_LEFT, false);
  EXPECT_EQ(buttons, platf::vmouse::BTN_LEFT);

  buttons = platf::vmouse::detail::apply_button_transition(buttons, platf::vmouse::BTN_RIGHT, false);
  EXPECT_EQ(buttons, platf::vmouse::BTN_LEFT | platf::vmouse::BTN_RIGHT);

  buttons = platf::vmouse::detail::apply_button_transition(buttons, platf::vmouse::BTN_LEFT, true);
  EXPECT_EQ(buttons, platf::vmouse::BTN_RIGHT);
}

TEST(VMouseReportTests, BuildOutputReportKeepsScrollBytesIndependent) {
  const auto report = platf::vmouse::detail::build_output_report(
    0,
    0,
    0,
    static_cast<int8_t>(-127),
    127);

  EXPECT_EQ(report[6], 0x81);
  EXPECT_EQ(report[7], 0x7F);
}

TEST(VMouseReportTests, DisconnectErrorsTriggerHandleClose) {
  EXPECT_TRUE(platf::vmouse::detail::should_close_on_write_error(ERROR_DEVICE_NOT_CONNECTED));
  EXPECT_TRUE(platf::vmouse::detail::should_close_on_write_error(ERROR_GEN_FAILURE));
  EXPECT_FALSE(platf::vmouse::detail::should_close_on_write_error(ERROR_INVALID_PARAMETER));
}

#endif
