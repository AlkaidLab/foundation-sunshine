#include "src/gamepad_selection.h"
#include <gtest/gtest.h>

TEST(GamepadSelection, SessionPreferencesDoNotOverwriteEachOther) {
  const std::string first = "ds5", second = "x360";
  EXPECT_EQ(input::select_gamepad_mode(first, true, 3, false), 4);
  EXPECT_EQ(input::select_gamepad_mode(second, true, 3, true), 2);
  // A later controller in the first session still inherits the first preference.
  EXPECT_EQ(input::select_gamepad_mode(first, true, 3, false), 4);
  EXPECT_EQ(input::select_gamepad_mode("", true, 3, false), 3);
}

TEST(GamepadSelection, AutoHintsArePerPlayerAndRespectExplicitModesAndHostPolicy) {
  EXPECT_EQ(input::select_gamepad_mode("", true, 1, true), 4);
  EXPECT_EQ(input::select_gamepad_mode("", true, 1, false), 1);
  EXPECT_EQ(input::select_gamepad_mode("", true, 2, true), 2);
  EXPECT_EQ(input::select_gamepad_mode("ds4", true, 1, true), 3);
  EXPECT_EQ(input::select_gamepad_mode("auto", true, 3, true), 4);
  EXPECT_EQ(input::select_gamepad_mode("ds5", false, 2, true), 2);
  EXPECT_EQ(input::select_gamepad_mode("invalid", true, 3, false), 3);
}
