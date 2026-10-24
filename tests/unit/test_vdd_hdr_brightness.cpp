#include <gtest/gtest.h>

#ifdef _WIN32
  #include "src/display_device/vdd_utils.h"

  #include <limits>

namespace {
  using display_device::vdd_utils::hdr_brightness_t;

  TEST(VddHdrBrightness, IdenticalCapabilitiesKeepTheSharedMonitor) {
    const hdr_brightness_t current { 1000.0f, 0.001f, 600.0f };
    EXPECT_TRUE(current.nearly_equal(current));
  }

  TEST(VddHdrBrightness, PeakAndFullFrameChangesInvalidateReuse) {
    const hdr_brightness_t current { 1000.0f, 0.001f, 600.0f };
    EXPECT_FALSE(current.nearly_equal({ 600.0f, 0.001f, 600.0f }));
    EXPECT_FALSE(current.nearly_equal({ 2000.0f, 0.001f, 600.0f }));
    EXPECT_FALSE(current.nearly_equal({ 1000.0f, 0.001f, 300.0f }));
  }

  TEST(VddHdrBrightness, BlackLevelChangesInvalidateReuse) {
    const hdr_brightness_t oled { 1000.0f, 0.001f, 600.0f };
    const hdr_brightness_t lcd { 1000.0f, 0.1f, 600.0f };
    EXPECT_FALSE(oled.nearly_equal(lcd));
    EXPECT_FALSE(lcd.nearly_equal(oled));
    EXPECT_FALSE(oled.nearly_equal({ 1000.0f, 0.01f, 600.0f }));
  }

  TEST(VddHdrBrightness, SmallMeasurementJitterDoesNotRebuildTheMonitor) {
    const hdr_brightness_t current { 1000.0f, 0.001f, 600.0f };
    const hdr_brightness_t noisy { 1010.0f, 0.00104f, 610.0f };
    EXPECT_TRUE(current.nearly_equal(noisy));
    EXPECT_TRUE(noisy.nearly_equal(current));
    EXPECT_TRUE((hdr_brightness_t { 1000.0f, 0.0f, 600.0f }).nearly_equal({ 1000.0f, 0.00001f, 600.0f }));
  }

  TEST(VddHdrBrightness, InvalidCapabilitiesCannotMatchTheCachedMonitor) {
    const hdr_brightness_t current { 1000.0f, 0.001f, 600.0f };
    EXPECT_FALSE(current.nearly_equal({ std::numeric_limits<float>::infinity(), 0.001f, 600.0f }));
    EXPECT_FALSE(current.nearly_equal({ 1000.0f, std::numeric_limits<float>::quiet_NaN(), 600.0f }));
  }

  TEST(VddHdrBrightness, CumulativeDriftIsComparedToTheProgrammedCapabilities) {
    const hdr_brightness_t programmed { 1000.0f, 0.001f, 600.0f };
    for (float peak = 1010.0f; peak <= 1050.0f; peak += 10.0f) {
      EXPECT_TRUE(programmed.nearly_equal({ peak, 0.001f, 600.0f }));
    }
    EXPECT_FALSE(programmed.nearly_equal({ 1060.0f, 0.001f, 600.0f }));
    EXPECT_FALSE(programmed.nearly_equal({ 1000.0f, 0.0012f, 600.0f }));
  }

  TEST(VddHdrBrightness, ClientSwitchDoesNotTreatDifferentCapabilitiesAsMeasurementNoise) {
    const hdr_brightness_t programmed { 1000.0f, 0.001f, 600.0f };
    const hdr_brightness_t different_client { 1010.0f, 0.00104f, 610.0f };
    EXPECT_TRUE(programmed.matches_for_reuse(different_client, false));
    EXPECT_FALSE(programmed.matches_for_reuse(different_client, true));
    EXPECT_TRUE(programmed.matches_for_reuse(programmed, true));
    EXPECT_FALSE(programmed.matches_for_reuse({ 1000.0f, 0.01f, 600.0f }, false));
  }
}  // namespace
#endif
