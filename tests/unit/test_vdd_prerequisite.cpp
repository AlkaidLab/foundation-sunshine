#include <gtest/gtest.h>

#include "src/display_device/vdd_utils.h"

#ifdef _WIN32

TEST(VddPrerequisiteSafety, ClassifiesEveryCoreState) {
  using display_device::vdd_utils::classify_vdd_state;

  EXPECT_EQ(classify_vdd_state(false, false, false, 0), "not_installed");
  EXPECT_EQ(classify_vdd_state(true, false, false, 14), "reboot_required");
  EXPECT_EQ(classify_vdd_state(true, false, false, 10), "unhealthy");
  EXPECT_EQ(classify_vdd_state(true, true, false, 0), "degraded");
  EXPECT_EQ(classify_vdd_state(true, true, true, 0), "ready");
}

TEST(VddPrerequisiteSafety, OnlyInstalledRunningDriversAreUsable) {
  display_device::vdd_utils::vdd_status_t status;
  EXPECT_FALSE(status.is_usable());

  status.installed = true;
  EXPECT_FALSE(status.is_usable());

  status.running = true;
  EXPECT_TRUE(status.is_usable());
}

#endif
