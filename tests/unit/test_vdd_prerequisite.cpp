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

TEST(VddPrerequisiteSafety, SharesPrerequisiteClassificationAcrossCallers) {
  using display_device::vdd_utils::classify_vdd_prerequisite;
  using display_device::vdd_utils::vdd_prerequisite_e;

  using display_device::vdd_utils::vdd_status_t;
  vdd_status_t status;
  status.installed = status.running = status.control_available = status.problem_code_valid = true;
  ASSERT_TRUE(status.is_usable());
  EXPECT_EQ(classify_vdd_prerequisite(status), vdd_prerequisite_e::usable);

  // Remove one prerequisite at a time; cumulative setup misses a weakened guard
  // when another still-false prerequisite keeps the result unavailable.
  const struct {
    const char *name;
    bool vdd_status_t::*field;
    vdd_prerequisite_e expected;
  } missing[] {
    { "installed", &vdd_status_t::installed, vdd_prerequisite_e::not_installed },
    { "problem_code_valid", &vdd_status_t::problem_code_valid, vdd_prerequisite_e::unavailable },
    { "running", &vdd_status_t::running, vdd_prerequisite_e::unavailable },
    { "control_available", &vdd_status_t::control_available, vdd_prerequisite_e::unavailable },
  };
  for (const auto &[name, field, expected] : missing) {
    SCOPED_TRACE(name);
    auto incomplete = status;
    incomplete.*field = false;
    EXPECT_FALSE(incomplete.is_usable());
    EXPECT_EQ(classify_vdd_prerequisite(incomplete), expected);
  }
}

#endif
