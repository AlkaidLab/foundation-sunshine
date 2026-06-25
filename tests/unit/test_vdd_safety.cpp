#include "../tests_common.h"

#ifdef _WIN32

#include "src/platform/windows/display_device/settings_topology.h"

TEST(VddBaselineSafety, DetectsVddOnlyTopology) {
  using namespace display_device;

  EXPECT_FALSE(is_vdd_only_topology({}, "vdd"));
  EXPECT_FALSE(is_vdd_only_topology({ { "vdd" } }, ""));
  EXPECT_FALSE(is_vdd_only_topology({ { "physical" } }, "vdd"));
  EXPECT_FALSE(is_vdd_only_topology({ { "vdd" }, { "physical" } }, "vdd"));
  EXPECT_TRUE(is_vdd_only_topology({ { "vdd" } }, "vdd"));
}

#endif
