/**
 * @file tests/unit/platform/windows/test_misc.cpp
 * @brief Test Windows timing helpers.
 */
#ifdef _WIN32

  #include <chrono>
  #include <cstdint>

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

#endif
