/**
 * @file tests/unit/test_niri_ipc.cpp
 * @brief Test the `niri msg --json outputs` parser.
 *
 * The payload shape is niri's own: an object keyed by output name, each entry
 * carrying a `modes` array in millihertz and a `current_mode` index (null when
 * the output is off). niri has no mode ids, no HDR field and no primary-output
 * concept, so the parser must not invent any of them.
 */
#include <src/platform/linux/niri_ipc.h>

#include "../tests_common.h"

#include <string>

namespace {
  using platf::niri::parse_outputs;

  const std::string kTwoOutputs = R"({
    "DP-1": {
      "name": "DP-1",
      "make": "Dell Inc.",
      "model": "DELL U2720Q",
      "serial": "8J2K3Q2",
      "physical_size": [600, 340],
      "modes": [
        { "width": 3840, "height": 2160, "refresh_rate": 59940, "is_preferred": true },
        { "width": 1920, "height": 1080, "refresh_rate": 60000, "is_preferred": false }
      ],
      "current_mode": 1,
      "is_custom_mode": false,
      "vrr_supported": true,
      "vrr_enabled": false,
      "logical": { "x": 0, "y": 0, "width": 1920, "height": 1080, "scale": 1.0, "transform": "Normal" }
    },
    "HDMI-A-1": {
      "name": "HDMI-A-1",
      "make": null,
      "model": null,
      "serial": null,
      "physical_size": null,
      "modes": [
        { "width": 2560, "height": 1440, "refresh_rate": 143981, "is_preferred": true }
      ],
      "current_mode": null,
      "is_custom_mode": false,
      "vrr_supported": false,
      "vrr_enabled": false,
      "logical": null
    }
  })";
}  // namespace

TEST(NiriIpc, ReportsEveryOutputWithItsName) {
  const auto outputs = parse_outputs(kTwoOutputs);

  ASSERT_EQ(outputs.size(), 2u);
  EXPECT_EQ(outputs[0].name, "DP-1");
  EXPECT_EQ(outputs[1].name, "HDMI-A-1");
}

TEST(NiriIpc, ConvertsMillihertzToHertz) {
  const auto outputs = parse_outputs(kTwoOutputs);

  ASSERT_EQ(outputs.size(), 2u);
  ASSERT_EQ(outputs[0].modes.size(), 2u);
  // 59940 mHz is the 59.94 Hz a user picks in the UI, not 60 Hz.
  EXPECT_DOUBLE_EQ(outputs[0].modes[0].refresh, 59.94);
  EXPECT_DOUBLE_EQ(outputs[0].modes[1].refresh, 60.0);
  EXPECT_DOUBLE_EQ(outputs[1].modes[0].refresh, 143.981);
}

TEST(NiriIpc, CarriesTheModeGeometryAndPreferredFlag) {
  const auto outputs = parse_outputs(kTwoOutputs);

  ASSERT_EQ(outputs.size(), 2u);
  ASSERT_EQ(outputs[0].modes.size(), 2u);
  EXPECT_EQ(outputs[0].modes[0].width, 3840u);
  EXPECT_EQ(outputs[0].modes[0].height, 2160u);
  EXPECT_TRUE(outputs[0].modes[0].preferred);
  EXPECT_FALSE(outputs[0].modes[1].preferred);
}

TEST(NiriIpc, MarksTheActiveModeByIndex) {
  const auto outputs = parse_outputs(kTwoOutputs);

  ASSERT_EQ(outputs.size(), 2u);
  // DP-1 is running its second mode.
  ASSERT_TRUE(outputs[0].current_mode.has_value());
  EXPECT_EQ(*outputs[0].current_mode, 1u);
}

TEST(NiriIpc, TreatsANullCurrentModeAsDisabled) {
  const auto outputs = parse_outputs(kTwoOutputs);

  ASSERT_EQ(outputs.size(), 2u);
  EXPECT_FALSE(outputs[1].current_mode.has_value());
  // Even an off output keeps advertising its modes: they are what enabling it
  // later can select.
  EXPECT_EQ(outputs[1].modes.size(), 1u);
}

TEST(NiriIpc, IgnoresAnIndexOutsideTheModeList) {
  // A compositor that reports a mode it did not list cannot be trusted; the
  // output is treated as off instead of pointing at nothing.
  const auto outputs = parse_outputs(R"({
    "eDP-1": {
      "name": "eDP-1",
      "modes": [ { "width": 1920, "height": 1080, "refresh_rate": 60000 } ],
      "current_mode": 4
    }
  })");

  ASSERT_EQ(outputs.size(), 1u);
  EXPECT_FALSE(outputs[0].current_mode.has_value());
}

TEST(NiriIpc, DefaultsMissingModeFieldsToZero) {
  const auto outputs = parse_outputs(R"({
    "eDP-1": {
      "name": "eDP-1",
      "modes": [ {} ],
      "current_mode": 0
    }
  })");

  ASSERT_EQ(outputs.size(), 1u);
  ASSERT_EQ(outputs[0].modes.size(), 1u);
  EXPECT_EQ(outputs[0].modes[0].width, 0u);
  EXPECT_EQ(outputs[0].modes[0].height, 0u);
  EXPECT_DOUBLE_EQ(outputs[0].modes[0].refresh, 0.0);
  EXPECT_FALSE(outputs[0].modes[0].preferred);
}

TEST(NiriIpc, SkipsEntriesThatAreNotObjects) {
  const auto outputs = parse_outputs(R"({
    "DP-1": { "name": "DP-1", "modes": [] },
    "DP-2": "gone"
  })");

  ASSERT_EQ(outputs.size(), 1u);
  EXPECT_EQ(outputs[0].name, "DP-1");
}

TEST(NiriIpc, RejectsAPayloadThatIsNotAnObject) {
  std::string error;
  const auto outputs = parse_outputs("[1, 2, 3]", &error);

  EXPECT_TRUE(outputs.empty());
  EXPECT_FALSE(error.empty());
}

TEST(NiriIpc, RejectsMalformedJson) {
  std::string error;
  const auto outputs = parse_outputs("{ \"DP-1\": ", &error);

  EXPECT_TRUE(outputs.empty());
  EXPECT_FALSE(error.empty());
}

TEST(NiriIpc, AcceptsAnEmptyObject) {
  std::string error;
  const auto outputs = parse_outputs("{}", &error);

  EXPECT_TRUE(outputs.empty());
  EXPECT_TRUE(error.empty());
}
