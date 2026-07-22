#include <gtest/gtest.h>

#include "src/display_device/vdd_utils.h"

#ifdef _WIN32

TEST(VddPrerequisiteSafety, ClassifiesEveryCoreState) {
  using display_device::vdd_utils::classify_vdd_state;

  EXPECT_EQ(classify_vdd_state(false, false, false, false, 0), "not_installed");
  EXPECT_EQ(classify_vdd_state(true, false, false, false, 0), "unknown");
  EXPECT_EQ(classify_vdd_state(true, false, false, true, 14), "reboot_required");
  EXPECT_EQ(classify_vdd_state(true, false, false, true, 10), "unhealthy");
  EXPECT_EQ(classify_vdd_state(true, true, false, true, 0), "degraded");
  EXPECT_EQ(classify_vdd_state(true, true, true, true, 0), "ready");
}

TEST(VddPrerequisiteSafety, OnlyInstalledRunningDriversAreUsable) {
  display_device::vdd_utils::vdd_status_t status;
  EXPECT_FALSE(status.is_usable());

  status.installed = true;
  EXPECT_FALSE(status.is_usable());

  status.running = true;
  EXPECT_FALSE(status.is_usable());

  status.problem_code_valid = true;
  EXPECT_TRUE(status.is_usable());
}

TEST(VddPrerequisiteSafety, SharesPrerequisiteClassificationAcrossCallers) {
  using display_device::vdd_utils::classify_vdd_prerequisite;
  using display_device::vdd_utils::vdd_prerequisite_e;

  display_device::vdd_utils::vdd_status_t status;
  EXPECT_EQ(classify_vdd_prerequisite(status), vdd_prerequisite_e::not_installed);

  status.installed = true;
  EXPECT_EQ(classify_vdd_prerequisite(status), vdd_prerequisite_e::unavailable);

  status.problem_code_valid = true;
  status.running = true;
  EXPECT_EQ(classify_vdd_prerequisite(status), vdd_prerequisite_e::usable);
}

#endif
