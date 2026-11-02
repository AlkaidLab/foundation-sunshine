#include "src/platform/windows/display_device/display_scale_steps.h"

#include <gtest/gtest.h>
#include <limits>

namespace {
  using namespace display_device::w_utils;

  TEST(DisplayScaleSteps, DecodesWindowsRelativeValuesIntoDesktopPercentages) {
    // Native example: recommended 175%, current one step lower, maximum 500%.
    // Assert against Windows percentages rather than a set/get round trip.
    const auto range = dpi_scale::decode_range(-3, -1, 8);
    ASSERT_TRUE(range);
    EXPECT_EQ(dpi_scale::percent_values[range->recommended_index], 175);
    EXPECT_EQ(dpi_scale::percent_values[range->current_index], 150);
    EXPECT_EQ(dpi_scale::percent_values[range->max_index], 500);

    const auto default_range = dpi_scale::decode_range(0, 1, 2);
    ASSERT_TRUE(default_range);
    EXPECT_EQ(dpi_scale::percent_values[default_range->recommended_index], 100);
    EXPECT_EQ(dpi_scale::percent_values[default_range->current_index], 125);
    EXPECT_EQ(dpi_scale::percent_values[default_range->max_index], 150);
  }

  TEST(DisplayScaleSteps, EncodesPercentagesAsNativeOffsetsFromRecommended175Percent) {
    EXPECT_EQ(dpi_scale::relative_from_percent(100, 3), -3);
    EXPECT_EQ(dpi_scale::relative_from_percent(125, 3), -2);
    EXPECT_EQ(dpi_scale::relative_from_percent(150, 3), -1);
    EXPECT_EQ(dpi_scale::relative_from_percent(175, 3), 0);
    EXPECT_EQ(dpi_scale::relative_from_percent(200, 3), 1);
    EXPECT_EQ(dpi_scale::relative_from_percent(500, 3), 8);
  }

  TEST(DisplayScaleSteps, RejectsDeviceScaleFactorValuesThatAreNotDesktopSteps) {
    for (const auto percent : { 120, 140, 160, 180 }) {
      EXPECT_FALSE(dpi_scale::relative_from_percent(percent, 0)) << percent;
    }
    EXPECT_FALSE(dpi_scale::relative_from_percent(150, -1));
    EXPECT_FALSE(dpi_scale::relative_from_percent(150, 12));
  }

  TEST(DisplayScaleSteps, RejectsInvalidNativeRangesWithoutOverflow) {
    EXPECT_FALSE(dpi_scale::decode_range(1, 0, 1));
    EXPECT_FALSE(dpi_scale::decode_range(-3, -4, 8));
    EXPECT_FALSE(dpi_scale::decode_range(-3, 9, 8));
    EXPECT_FALSE(dpi_scale::decode_range(-3, 0, 9));
    EXPECT_FALSE(dpi_scale::decode_range(-3, -1, -1));
    EXPECT_FALSE(dpi_scale::decode_range(std::numeric_limits<std::int32_t>::min(), 0, 0));
    EXPECT_FALSE(dpi_scale::decode_range(-1, std::numeric_limits<std::int32_t>::max(), 1));
  }
}  // namespace
