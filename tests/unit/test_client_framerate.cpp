#include "src/streaming/client_framerate.h"

#include <gtest/gtest.h>

#include <limits>

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
