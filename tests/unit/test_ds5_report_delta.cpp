/**
 * @file tests/unit/test_ds5_report_delta.cpp
 * @brief Change detection between two DualSense input reports.
 */
#include <gtest/gtest.h>

#include <array>

#include "src/ds5/report_delta.h"

namespace {
  using ds5::report_delta;
  using report_t = std::array<std::uint8_t, 64>;

  report_t
  idle() {
    report_t report {};
    report[0] = 1;
    report[1] = report[2] = report[3] = report[4] = 128;
    report[8] = 8;
    return report;
  }

  report_delta
  delta(const report_t &before, const report_t &after) { return ds5::input_report_delta(before, after); }
}  // namespace

TEST(Ds5ReportDelta, TheSameReportIsNoChange) {
  EXPECT_EQ(delta(idle(), idle()), report_delta::none);
}

TEST(Ds5ReportDelta, TheSequenceCounterAndTheSensorTimestampAreNotAChange) {
  auto next = idle();
  next[7] = 9;
  next[28] = 1;
  next[29] = 2;
  next[30] = 3;
  next[31] = 4;
  EXPECT_EQ(delta(idle(), next), report_delta::none);
}

TEST(Ds5ReportDelta, SticksTriggersMotionTouchAndStatusAreStreamChanges) {
  for (const std::size_t index : { 1, 4, 5, 6, 16, 21, 22, 27, 32, 33, 40, 53, 54 }) {
    auto next = idle();
    next[index] ^= 0x10;
    EXPECT_EQ(delta(idle(), next), report_delta::stream) << "byte " << index;
  }
}

TEST(Ds5ReportDelta, ButtonBytesAreButtonChanges) {
  for (const std::size_t index : { 8, 9, 10 }) {
    auto next = idle();
    next[index] ^= 0x01;
    EXPECT_EQ(delta(idle(), next), report_delta::buttons) << "byte " << index;
  }
}

TEST(Ds5ReportDelta, AButtonWinsOverAStreamChangeInTheSameReport) {
  auto next = idle();
  next[1] = 200;
  next[8] ^= 0x20;
  EXPECT_EQ(delta(idle(), next), report_delta::buttons);
}

TEST(Ds5ReportDelta, ShortReportsAreComparedOverTheirCommonLength) {
  const std::array<std::uint8_t, 4> a { 1, 128, 128, 128 };
  const std::array<std::uint8_t, 6> b { 1, 128, 128, 128, 0, 0 };
  EXPECT_EQ(ds5::input_report_delta(a, b), report_delta::none);
  const std::array<std::uint8_t, 4> c { 1, 128, 129, 128 };
  EXPECT_EQ(ds5::input_report_delta(a, c), report_delta::stream);
}
