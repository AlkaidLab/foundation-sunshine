#include "src/streaming/client_framerate.h"

#include <gtest/gtest.h>

#include <limits>

namespace {

  constexpr streaming::frame_rate_rational_t
  rate(int num, int den) {
    return { num, den };
  }

}  // namespace

TEST(ClientFramerate, UsableRatesPassThrough) {
  EXPECT_EQ(streaming::usable_client_framerate(1), 1);
  EXPECT_EQ(streaming::usable_client_framerate(30), 30);
  EXPECT_EQ(streaming::usable_client_framerate(60), 60);
  EXPECT_EQ(streaming::usable_client_framerate(165), 165);
  EXPECT_EQ(streaming::usable_client_framerate(streaming::max_client_framerate), streaming::max_client_framerate);
}

TEST(ClientFramerate, ZeroAndNegativeNeverReachTheCapturePacing) {
  EXPECT_EQ(streaming::usable_client_framerate(0), streaming::default_client_framerate);
  EXPECT_EQ(streaming::usable_client_framerate(-1), streaming::default_client_framerate);
  EXPECT_EQ(streaming::usable_client_framerate(std::numeric_limits<int>::min()), streaming::default_client_framerate);
  EXPECT_GT(streaming::usable_client_framerate(0), 0);
}

TEST(ClientFramerate, AbsurdRatesAreCapped) {
  EXPECT_EQ(streaming::usable_client_framerate(streaming::max_client_framerate + 1), streaming::max_client_framerate);
  EXPECT_EQ(streaming::usable_client_framerate(std::numeric_limits<int>::max()), streaming::max_client_framerate);
}

TEST(ClientFramerate, FractionalRatesRefineTheEffectiveRate) {
  EXPECT_EQ(streaming::effective_frame_rate(5994, 60), rate(5994, 100));
  EXPECT_EQ(streaming::effective_frame_rate(2997, 30), rate(2997, 100));
  EXPECT_EQ(streaming::effective_frame_rate(11988, 120), rate(11988, 100));
  EXPECT_EQ(streaming::effective_frame_rate(6000, 60), rate(60, 1));
}

TEST(ClientFramerate, FractionalRateNeverExceedsTheEffectiveMax) {
  // 61.00 and 61.19 stayed inside the old 2% window above 60 and streamed as 61/1.
  EXPECT_EQ(streaming::effective_frame_rate(6100, 60), rate(60, 1));
  EXPECT_EQ(streaming::effective_frame_rate(6119, 60), rate(60, 1));
  EXPECT_EQ(streaming::effective_frame_rate(6050, 60), rate(60, 1));
  EXPECT_EQ(streaming::effective_frame_rate(100094, 1000), rate(1000, 1));
}

TEST(ClientFramerate, FractionalRateNeverExceedsTheReplacementRate) {
  // maxFPS 0 falls back to 60 and maxFPS above 1000 is capped at 1000; the fractional
  // announcement cannot lift the stream above the rate that replaced the request.
  EXPECT_EQ(streaming::effective_frame_rate(6100, streaming::usable_client_framerate(0)), rate(60, 1));
  EXPECT_EQ(streaming::effective_frame_rate(101000, streaming::usable_client_framerate(1010)), rate(1000, 1));
  EXPECT_EQ(streaming::effective_frame_rate(6100, -1), rate(60, 1));
}

TEST(ClientFramerate, FractionalRateOutsideTheWindowFallsBackToTheIntegerRate) {
  EXPECT_EQ(streaming::effective_frame_rate(5000, 60), rate(60, 1));
  EXPECT_EQ(streaming::effective_frame_rate(5880, 60), rate(60, 1));  // exactly 2% below
  EXPECT_EQ(streaming::effective_frame_rate(5881, 60), rate(5881, 100));
  EXPECT_EQ(streaming::effective_frame_rate(0, 60), rate(60, 1));
  EXPECT_EQ(streaming::effective_frame_rate(-5994, 60), rate(60, 1));
}

TEST(ClientFramerate, EffectiveRateNeverExceedsItsBound) {
  for (int effective = 1; effective <= streaming::max_client_framerate; ++effective) {
    const int center = effective * 100;
    for (int x100 = center - center / 25; x100 <= center + center / 25; ++x100) {  // ±4%, covers the window
      const auto result = streaming::effective_frame_rate(x100, effective);
      ASSERT_GT(result.den, 0);
      ASSERT_GT(result.num, 0);
      ASSERT_LE(static_cast<double>(result.num) / result.den, static_cast<double>(effective) + 1e-9);
    }
    for (const int x100 : { 0, 1, 5994, 6100, 101000, center, center + 1, center * 2 }) {
      const auto result = streaming::effective_frame_rate(x100, effective);
      ASSERT_GT(result.den, 0);
      ASSERT_GT(result.num, 0);
      ASSERT_LE(static_cast<double>(result.num) / result.den, static_cast<double>(effective) + 1e-9);
    }
  }
}
