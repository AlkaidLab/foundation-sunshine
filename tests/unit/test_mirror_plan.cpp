/**
 * @file tests/unit/test_mirror_plan.cpp
 * @brief Tests for the KScreen mirroring plan.
 */
// local includes
#include "src/platform/linux/mirror_plan.h"

// standard includes
#include <string>
#include <vector>

// third-party includes
#include <gtest/gtest.h>

namespace {
  using platf::mirror::output_t;

  std::vector<output_t> outputs(std::vector<output_t> list) {
    return list;
  }
}  // namespace

TEST(MirrorPlan, AsksForNothingWhenAlreadyMirrored) {
  const auto current = outputs({ { "eDP-1", 1, true, 0 }, { "HDMI-2", 70, true, 1 } });

  EXPECT_EQ(platf::mirror::plan(current, { { "eDP-1", "HDMI-2" } }), (std::vector<std::string> {}));
}

TEST(MirrorPlan, MirrorsTheGroupOntoItsFirstMember) {
  const auto current = outputs({ { "eDP-1", 1, true, 0 }, { "HDMI-2", 70, true, 0 } });

  EXPECT_EQ(platf::mirror::plan(current, { { "eDP-1", "HDMI-2" } }),
    (std::vector<std::string> { "output.HDMI-2.mirror.eDP-1" }));
}

TEST(MirrorPlan, ReleasesAnOutputThatShouldStandAloneAgain) {
  const auto current = outputs({ { "eDP-1", 1, true, 0 }, { "HDMI-2", 70, true, 1 } });

  EXPECT_EQ(platf::mirror::plan(current, { { "eDP-1" }, { "HDMI-2" } }),
    (std::vector<std::string> { "output.HDMI-2.mirror.none" }));
}

TEST(MirrorPlan, RetargetsAMirrorThatMoved) {
  const auto current = outputs({ { "eDP-1", 1, true, 0 }, { "DP-2", 5, true, 0 }, { "HDMI-2", 70, true, 1 } });

  EXPECT_EQ(platf::mirror::plan(current, { { "DP-2", "HDMI-2" } }),
    (std::vector<std::string> { "output.HDMI-2.mirror.DP-2" }));
}

TEST(MirrorPlan, KeepsTheShapeOfAnExtendedDesktop) {
  const auto current = outputs({ { "eDP-1", 1, true, 0 }, { "HDMI-2", 70, true, 0 } });

  EXPECT_EQ(platf::mirror::plan(current, { { "eDP-1" }, { "HDMI-2" } }), (std::vector<std::string> {}));
}

TEST(MirrorPlan, IgnoresAMirrorOnADisabledOutput) {
  // A disabled output keeps a stale relation in the compositor, but touching it
  // would fail the whole atomic request; enabling it clears the relation.
  const auto current = outputs({ { "eDP-1", 1, true, 0 }, { "HDMI-2", 70, false, 1 } });

  EXPECT_EQ(platf::mirror::plan(current, { { "eDP-1" } }), (std::vector<std::string> {}));
}

TEST(MirrorPlan, RefusesATopologyTheCompositorDoesNotReport) {
  const auto current = outputs({ { "eDP-1", 1, true, 0 } });

  EXPECT_FALSE(platf::mirror::plan(current, { { "eDP-1", "HDMI-2" } }).has_value());
}

TEST(MirrorPlan, SwapsWhichOutputCarriesTheRelation) {
  // HDMI-2 mirrored eDP-1 before; now eDP-1 has to mirror HDMI-2. The relation
  // lives on the mirrored output, so the old one is released first.
  const auto current = outputs({ { "eDP-1", 1, true, 0 }, { "HDMI-2", 70, true, 1 } });

  EXPECT_EQ(platf::mirror::plan(current, { { "HDMI-2", "eDP-1" } }),
    (std::vector<std::string> { "output.HDMI-2.mirror.none", "output.eDP-1.mirror.HDMI-2" }));
}
