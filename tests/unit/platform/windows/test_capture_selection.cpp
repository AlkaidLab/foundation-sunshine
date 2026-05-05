#include "src/platform/windows/capture_selection.h"

#include <gtest/gtest.h>

TEST(WindowsCaptureSelectionTests, CursorPlanePrefersWgcWhenAutoAndUserSession) {
  auto order = platf::dxgi::windows_capture_try_order("", false, true);

  ASSERT_EQ(order.size(), 2);
  EXPECT_EQ(order[0], "wgc");
  EXPECT_EQ(order[1], "ddx");
}

TEST(WindowsCaptureSelectionTests, CursorPlaneFallsBackToDdxInServiceMode) {
  auto order = platf::dxgi::windows_capture_try_order("", true, true);

  ASSERT_EQ(order.size(), 1);
  EXPECT_EQ(order[0], "ddx");
}

TEST(WindowsCaptureSelectionTests, ExplicitCaptureSettingIsRespected) {
  auto order = platf::dxgi::windows_capture_try_order("ddx", false, true);

  ASSERT_EQ(order.size(), 1);
  EXPECT_EQ(order[0], "ddx");
}
