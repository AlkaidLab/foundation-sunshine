#include "src/streaming/display_recovery.h"

#include <gtest/gtest.h>

TEST(DisplayRecovery, VirtualTargetWaitsUntilItsNewDisplayAppears) {
  const std::vector<std::string> absent { "physical" };
  EXPECT_FALSE(streaming::select_recovery_display(absent, "", "physical", true));
  EXPECT_FALSE(streaming::select_recovery_display(absent, "old-virtual", "physical", true));
  const std::vector<std::string> restored { "physical", "new-virtual" };
  EXPECT_EQ(streaming::select_recovery_display(restored, "new-virtual", "physical", true), "new-virtual");
}

TEST(DisplayRecovery, PhysicalFallbackAndManualSwitchRemainAvailable) {
  const std::vector<std::string> displays { "physical", "other" };
  EXPECT_EQ(streaming::select_recovery_display(displays, "missing", "physical", false), "physical");
  EXPECT_EQ(streaming::select_recovery_display(displays, "other", "physical", false), "other");
}

TEST(DisplayRecovery, FailedOrSkippedConfigurationDoesNotCommit) {
  int width = 100;
  EXPECT_FALSE(streaming::commit_display_resolution(true, false, [&] { width = 200; }));
  EXPECT_FALSE(streaming::commit_display_resolution(false, true, [&] { width = 200; }));
  EXPECT_EQ(width, 100);
  EXPECT_TRUE(streaming::commit_display_resolution(true, true, [&] { width = 200; }));
  EXPECT_EQ(width, 200);
}
