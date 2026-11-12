/**
 * @file tests/unit/test_vdd_primary_plan.cpp
 * @brief Tests for the virtual-display primary role rules.
 */
// local includes
#include "src/platform/linux/vdd_primary_plan.h"

// standard includes
#include <string>
#include <vector>

// third-party includes
#include <gtest/gtest.h>

namespace {
  using platf::vdd_primary::hint_t;
  using platf::vdd_primary::role_e;

  std::vector<hint_t> hints(std::vector<hint_t> list) {
    return list;
  }
}  // namespace

TEST(VddPrimaryPlan, PromotesTheKnownPrimaryAndDemotesTheRest) {
  const std::vector<std::string> physicals { "HDMI-A-1", "DP-2" };

  // The primary is the *second* connector in enumeration order: the plan must
  // follow the recorded role, not the order it walks the physicals in.
  EXPECT_EQ(platf::vdd_primary::secondary_plan(physicals, "DP-1", "DP-2"),
    hints({ { "DP-2", role_e::primary }, { "HDMI-A-1", role_e::secondary }, { "DP-1", role_e::secondary } }));
}

TEST(VddPrimaryPlan, PromotesNothingWhenThePrimaryIsUnknown) {
  const std::vector<std::string> physicals { "HDMI-A-1", "DP-2" };

  // Handing the role to a physical picked by enumeration order is exactly what
  // this rule removes, so an unknown primary leaves the physical side alone and
  // only the virtual display is demoted.
  EXPECT_EQ(platf::vdd_primary::secondary_plan(physicals, "DP-1", {}),
    hints({ { "DP-1", role_e::secondary } }));
}

TEST(VddPrimaryPlan, IgnoresAPrimaryThatIsNoLongerConnected) {
  const std::vector<std::string> physicals { "HDMI-A-1" };

  // eDP-1 was primary when the session started and has been unplugged since.
  EXPECT_EQ(platf::vdd_primary::secondary_plan(physicals, "DP-1", "eDP-1"),
    hints({ { "DP-1", role_e::secondary } }));
}

TEST(VddPrimaryPlan, NeverPromotesTheVirtualDisplay) {
  const std::vector<std::string> physicals { "HDMI-A-1" };

  // A stale record naming the virtual display cannot turn it into the primary.
  EXPECT_EQ(platf::vdd_primary::secondary_plan(physicals, "DP-1", "DP-1"),
    hints({ { "DP-1", role_e::secondary } }));
}

TEST(VddPrimaryPlan, TeardownRestoresTheRememberedPrimary) {
  const std::vector<std::string> physicals { "HDMI-A-1", "DP-2" };

  // No virtual display left to demote: the remembered screen takes the role
  // back and the other physical stays extended.
  EXPECT_EQ(platf::vdd_primary::secondary_plan(physicals, {}, "DP-2"),
    hints({ { "DP-2", role_e::primary }, { "HDMI-A-1", role_e::secondary } }));
}

TEST(VddPrimaryPlan, TeardownWithoutARememberedPrimaryTouchesNothing) {
  EXPECT_EQ(platf::vdd_primary::secondary_plan({ "HDMI-A-1" }, {}, {}), hints({}));
}

TEST(VddPrimaryPlan, DemotesTheVirtualDisplayWithoutPhysicals) {
  // Headless host: the virtual display is the only output, so demoting it is
  // the whole plan.
  EXPECT_EQ(platf::vdd_primary::secondary_plan({}, "DP-1", {}),
    hints({ { "DP-1", role_e::secondary } }));
}
