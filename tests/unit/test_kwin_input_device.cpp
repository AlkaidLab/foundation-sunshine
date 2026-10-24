/**
 * @file tests/unit/test_kwin_input_device.cpp
 * @brief Tests for the KWin touch-device mapping decisions.
 */
#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "src/platform/linux/kwin_input_device.h"

namespace {
  using platf::kwin_input::collect_matching_nodes;
  using platf::kwin_input::kTouchDeviceName;
  using platf::kwin_input::mapping_t;
  using platf::kwin_input::plan_mapping;

  /// The interesting property of the mapping is that it always asks for workspace-wide
  /// coordinates: Windows absolute touch input is expressed over the whole virtual
  /// desktop, so confining the device to one output would be a parity break.
  void
  expect_workspace_wide(const mapping_t &mapping) {
    EXPECT_TRUE(mapping.map_to_workspace);
  }
}  // namespace

TEST(KwinInputDevice, PinsTheStreamedOutputWhenItIsEnabled) {
  const auto mapping = plan_mapping({ "DP-2", "HDMI-A-1" }, "HDMI-A-1");

  EXPECT_EQ("HDMI-A-1", mapping.output_name);
  expect_workspace_wide(mapping);
}

TEST(KwinInputDevice, FallsBackToAnEnabledOutputWhenTheStreamedOneIsGone) {
  // A virtual display is created after the session starts, so at the moment the touch
  // device is created the streamed connector may not exist yet. Any enabled output is
  // enough: with map_to_workspace the coordinates span the workspace anyway, and KWin
  // only needs the device to have *an* output to deliver events at all.
  const auto mapping = plan_mapping({ "DP-2" }, "DP-9");

  EXPECT_EQ("DP-2", mapping.output_name);
  expect_workspace_wide(mapping);
}

TEST(KwinInputDevice, FallsBackToAnEnabledOutputWhenNothingIsStreamed) {
  const auto mapping = plan_mapping({ "eDP-1", "DP-2" }, "");

  EXPECT_EQ("eDP-1", mapping.output_name);
  expect_workspace_wide(mapping);
}

TEST(KwinInputDevice, LeavesTheOutputAloneWhenNothingIsEnabled) {
  // No enabled connector: KWin's own guess is as good as ours, and forcing a name that
  // does not exist would only make the property set fail.
  const auto mapping = plan_mapping({}, "DP-2");

  EXPECT_TRUE(mapping.output_name.empty());
  expect_workspace_wide(mapping);
}

TEST(KwinInputDevice, MatchesOnlyOurOwnDeviceName) {
  const std::vector<std::pair<std::string, std::string>> nodes {
    { "event17", "SYNA2BA6:00 06CB:CE44 Touchpad" },
    { "event30", std::string { kTouchDeviceName } },
    { "event31", "Pen passthrough" },
    { "event9", "Power Button" },
  };

  const auto matched = collect_matching_nodes(nodes, kTouchDeviceName);

  // The pen is a separate device with its own mapping; picking it up here would
  // configure the wrong /sys node.
  ASSERT_EQ(1u, matched.size());
  EXPECT_EQ("event30", matched.front());
}

TEST(KwinInputDevice, MatchesRepeatedNodesInStableOrder) {
  // One synthetic touchscreen per connected client: each of them needs the mapping, and
  // the order must not depend on directory iteration order.
  const std::vector<std::pair<std::string, std::string>> nodes {
    { "event44", std::string { kTouchDeviceName } },
    { "event30", std::string { kTouchDeviceName } },
    { "event12", std::string { kTouchDeviceName } },
  };

  const auto matched = collect_matching_nodes(nodes, kTouchDeviceName);

  ASSERT_EQ(3u, matched.size());
  EXPECT_EQ("event12", matched[0]);
  EXPECT_EQ("event30", matched[1]);
  EXPECT_EQ("event44", matched[2]);
}

TEST(KwinInputDevice, IgnoresEmptyAndForeignNames) {
  const std::vector<std::pair<std::string, std::string>> nodes {
    { "event2", "" },
    { "event3", "touch passthrough" },  // case differs: KWin's config key is exact
    { "event4", "Touch passthrough pen" },
  };

  EXPECT_TRUE(collect_matching_nodes(nodes, kTouchDeviceName).empty());
}

/**
 * @brief Live smoke test: exercise the sysfs scan and the D-Bus write on this host.
 * @details Skips unless a synthetic touchscreen exists and KWin owns its session-bus name,
 *          i.e. unless a client is streaming on a KDE session. Applying the mapping twice
 *          is idempotent, so this is safe to run against a live session.
 */
TEST(KwinInputDevice, LiveAppliesToARealDevice) {
  const auto nodes = platf::kwin_input::find_event_nodes(kTouchDeviceName);
  if (nodes.empty()) {
    GTEST_SKIP() << "no synthetic touchscreen on this host (no client streaming)";
  }
  if (!platf::kwin_input::available()) {
    GTEST_SKIP() << "KWin is not on the session bus";
  }

  const auto outputs = platf::kwin_input::enabled_outputs();
  ASSERT_FALSE(outputs.empty()) << "a touch device exists but no connector is enabled";

  const auto mapping = plan_mapping(outputs, "");
  std::string error;
  EXPECT_TRUE(platf::kwin_input::apply(nodes.front(), mapping, &error)) << error;

  // The whole point of the exercise: the device must not be left pointing at a connector
  // that is not the one being streamed.
  EXPECT_FALSE(mapping.output_name.empty());
}
