/**
 * @file tests/unit/platform/windows/test_misc.cpp
 * @brief Test Windows timing helpers.
 */
#ifdef _WIN32

  #include <chrono>
  #include <cstdint>
  #include <limits>

  #include <src/platform/windows/misc.h>

  #include "../../../tests_common.h"

using namespace std::chrono_literals;

TEST(WindowsQpcTiming, ConvertsTicksUsingTicksPerSecond) {
  constexpr std::int64_t frequency = 10'000'000;

  EXPECT_EQ(platf::qpc_ticks_to_duration(100'000, frequency), 10ms);
  EXPECT_EQ(platf::qpc_ticks_to_duration(-100'000, frequency), -10ms);
}

TEST(WindowsQpcTiming, PreservesSubMillisecondPrecision) {
  constexpr std::int64_t frequency = 10'000'000;

  EXPECT_EQ(platf::qpc_ticks_to_duration(1, frequency), 100ns);
  EXPECT_EQ(platf::qpc_ticks_to_duration(15, frequency), 1500ns);
}

TEST(WindowsQpcTiming, RejectsInvalidFrequency) {
  EXPECT_EQ(platf::qpc_ticks_to_duration(123, 0), 0ns);
  EXPECT_EQ(platf::qpc_ticks_to_duration(123, -1), 0ns);
}

TEST(WindowsQpcTiming, SaturatesExtremeTicksAtLowFrequency) {
  EXPECT_EQ(platf::qpc_ticks_to_duration(2, 1), 2s);
  EXPECT_EQ(platf::qpc_ticks_to_duration(-2, 1), -2s);
  EXPECT_EQ(
    platf::qpc_ticks_to_duration(std::numeric_limits<std::int64_t>::max(), 1),
    std::chrono::nanoseconds::max());
  EXPECT_EQ(
    platf::qpc_ticks_to_duration(std::numeric_limits<std::int64_t>::min(), 1),
    std::chrono::nanoseconds::min());
}

TEST(WindowsQpcTiming, SaturatesExtremeCounterDifferences) {
  EXPECT_EQ(
    platf::qpc_time_difference(
      std::numeric_limits<std::int64_t>::max(),
      std::numeric_limits<std::int64_t>::min()),
    std::chrono::nanoseconds::max());
  EXPECT_EQ(
    platf::qpc_time_difference(
      std::numeric_limits<std::int64_t>::min(),
      std::numeric_limits<std::int64_t>::max()),
    std::chrono::nanoseconds::min());
}

#endif
