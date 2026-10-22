/**
 * @file tests/unit/test_wlr_randr.cpp
 * @brief Test the wlr-randr listing parser and argument builder.
 *
 * wlr-randr prints the state with its own printf formats and takes a whole
 * configuration back as argv, so the contract has two halves that a compositor
 * is not needed to check. The samples below are byte-for-byte what the tool
 * emits (`print_state` in wlr-randr: a head line in column zero, two-space
 * field indents, modes indented four spaces, "Position"/"Transform"/"Scale"
 * only for enabled heads).
 */
#include <src/platform/linux/wlr_randr.h>

#include "../tests_common.h"

#include <string>
#include <vector>

namespace {
  using platf::wlr_randr::build_args;
  using platf::wlr_randr::mode_spec;
  using platf::wlr_randr::parse_state;
  using platf::wlr_randr::request_t;

  const std::string kTwoHeads = R"WLR(DP-1 "Dell Inc. DELL U2720Q (DP-1)"
  Physical size: 600x340 mm
  Enabled: yes
  Modes:
    3840x2160 px, 59.940000 Hz
    3840x2160 px, 60.000000 Hz (preferred)
    1920x1080 px, 50.000000 Hz
    1920x1080 px, 60.000000 Hz (current)
  Position: 0,0
  Transform: normal
  Scale: 1.000000
HDMI-A-1 "Ancor Communications Inc VG248 (HDMI-A-1)"
  Physical size: 531x299 mm
  Enabled: no
  Modes:
    1920x1080 px, 60.000000 Hz (preferred)
    1920x1080 px, 50.000000 Hz
)WLR";

  const std::string kNoModes = R"WLR(HEADLESS-1 "Headless output"
  Enabled: no
)WLR";
}  // namespace

TEST(WlrRandr, ParsesHeadFields) {
  const auto state = parse_state(kTwoHeads);

  ASSERT_EQ(state.heads.size(), 2u);

  const auto &head = state.heads[0];
  EXPECT_EQ(head.name, "DP-1");
  EXPECT_EQ(head.description, "Dell Inc. DELL U2720Q (DP-1)");
  EXPECT_TRUE(head.enabled);
  EXPECT_EQ(head.phys_width, 600u);
  EXPECT_EQ(head.phys_height, 340u);
  EXPECT_EQ(head.x, 0);
  EXPECT_EQ(head.y, 0);
  EXPECT_EQ(head.transform, "normal");
  EXPECT_DOUBLE_EQ(head.scale, 1.0);

  const auto &second = state.heads[1];
  EXPECT_EQ(second.name, "HDMI-A-1");
  EXPECT_EQ(second.description, "Ancor Communications Inc VG248 (HDMI-A-1)");
  EXPECT_FALSE(second.enabled);
  EXPECT_EQ(second.phys_width, 531u);
}

TEST(WlrRandr, ParsesModeListWithFlags) {
  const auto state = parse_state(kTwoHeads);
  ASSERT_EQ(state.heads.size(), 2u);

  const auto &modes = state.heads[0].modes;
  ASSERT_EQ(modes.size(), 4u);

  EXPECT_EQ(modes[0].width, 3840u);
  EXPECT_EQ(modes[0].height, 2160u);
  EXPECT_DOUBLE_EQ(modes[0].refresh, 59.94);
  EXPECT_FALSE(modes[0].current);
  EXPECT_FALSE(modes[0].preferred);
  // The spec is what the tool accepts back as --mode, including millihertz.
  EXPECT_EQ(modes[0].spec, "3840x2160@59.94");

  EXPECT_TRUE(modes[1].preferred);
  EXPECT_EQ(modes[1].spec, "3840x2160@60");

  EXPECT_TRUE(modes[3].current);
  EXPECT_FALSE(modes[3].preferred);
  EXPECT_EQ(modes[3].spec, "1920x1080@60");

  // A disabled head still reports its modes; only its geometry is absent.
  ASSERT_EQ(state.heads[1].modes.size(), 2u);
  EXPECT_TRUE(state.heads[1].modes[0].preferred);
}

TEST(WlrRandr, DisabledHeadKeepsDefaultsForGeometry) {
  const auto state = parse_state(kNoModes);

  ASSERT_EQ(state.heads.size(), 1u);
  const auto &head = state.heads[0];
  EXPECT_EQ(head.name, "HEADLESS-1");
  EXPECT_FALSE(head.enabled);
  EXPECT_TRUE(head.modes.empty());
  // No Position/Transform/Scale line is printed for a disabled head, so the
  // parser must leave what the tool itself defaults to.
  EXPECT_EQ(head.x, 0);
  EXPECT_EQ(head.y, 0);
  EXPECT_EQ(head.transform, "normal");
  EXPECT_DOUBLE_EQ(head.scale, 1.0);
}

TEST(WlrRandr, IgnoresUnknownFieldsAndEmptyOutput) {
  EXPECT_TRUE(parse_state("").heads.empty());
  EXPECT_TRUE(parse_state("\n\n").heads.empty());

  // A field a future wlr-randr may add must not derail the fields after it.
  const auto state = parse_state(R"WLR(DP-2 "Vendor Panel"
  Enabled: yes
  Adaptive sync: enabled
  Position: 1920,1080
  Transform: flipped-90
  Scale: 1.500000
)");

  ASSERT_EQ(state.heads.size(), 1u);
  EXPECT_EQ(state.heads[0].x, 1920);
  EXPECT_EQ(state.heads[0].y, 1080);
  EXPECT_EQ(state.heads[0].transform, "flipped-90");
  EXPECT_DOUBLE_EQ(state.heads[0].scale, 1.5);
}

TEST(WlrRandr, BuildsEnableAndDisableArguments) {
  EXPECT_EQ(build_args({ { "DP-1", true, {} } }), "--output 'DP-1' --on");
  EXPECT_EQ(build_args({ { "DP-1", false, {} } }), "--output 'DP-1' --off");
  EXPECT_EQ(build_args({ { "DP-1", true, "1920x1080@60" } }),
            "--output 'DP-1' --on --mode '1920x1080@60'");

  // Several heads in one invocation: wlr-randr re-applies every head it read,
  // so a request only has to name the ones it changes.
  EXPECT_EQ(build_args({ { "DP-1", true, "3840x2160@59.94" }, { "HDMI-A-1", false, {} } }),
            "--output 'DP-1' --on --mode '3840x2160@59.94' --output 'HDMI-A-1' --off");
}

TEST(WlrRandr, QuotesOutputNamesForTheShell) {
  // Sunshine runs its helpers through /bin/sh, so a name the compositor
  // reported has to survive the shell verbatim.
  EXPECT_EQ(build_args({ { "DP-1 (left)", true, {} } }), "--output 'DP-1 (left)' --on");
  EXPECT_EQ(build_args({ { "it's", true, {} } }), R"WLR(--output 'it'\''s' --on)WLR");
}

TEST(WlrRandr, DryRunOnlyMarksRealRequests) {
  EXPECT_EQ(build_args({ { "DP-1", true, "1920x1080@60" } }, true),
            "--output 'DP-1' --on --mode '1920x1080@60' --dryrun");
  // With no request there is nothing to test, and the flag alone would make
  // wlr-randr apply an empty configuration instead of printing the state.
  EXPECT_TRUE(build_args({}, true).empty());
  EXPECT_TRUE(build_args({ request_t { std::string {}, true, std::string {} } }, true).empty());
}

TEST(WlrRandr, ModeSpecRoundTripsMillihertz) {
  // The tool converts a rate to millihertz with round(hz * 1000) and matches it
  // against the advertised modes, so the printed spec must survive that trip.
  EXPECT_EQ(mode_spec(1920, 1080, 59.94), "1920x1080@59.94");
  EXPECT_EQ(mode_spec(1920, 1080, 60.0), "1920x1080@60");
  EXPECT_EQ(mode_spec(2560, 1440, 143.981), "2560x1440@143.981");
  EXPECT_EQ(mode_spec(2560, 1440, 23.976), "2560x1440@23.976");
  // No rate reported: width and height only.
  EXPECT_EQ(mode_spec(1280, 720, 0.0), "1280x720");
}

TEST(WlrRandr, ParsedSpecsAreAcceptedBackAsArguments) {
  // The round trip that matters: read a listing, hand a chosen mode back.
  const auto state = parse_state(kTwoHeads);
  ASSERT_EQ(state.heads.size(), 2u);

  const auto &chosen = state.heads[0].modes[0];
  const auto args = build_args({ { state.heads[0].name, true, chosen.spec } });
  EXPECT_EQ(args, "--output 'DP-1' --on --mode '3840x2160@59.94'");
}

TEST(WlrRandr, TellsUnsupportedCompositorsFromRealFailures) {
  // The two strings below are the ones a wlr-randr built from upstream source
  // actually printed here: the first in a session whose compositor has no
  // output manager, the second with no compositor at all. Both mean "not our
  // session"; anything else is an error worth surfacing.
  EXPECT_TRUE(platf::wlr_randr::failure_means_unsupported(
    "compositor doesn't support wlr-output-management-unstable-v1\n"));
  EXPECT_TRUE(platf::wlr_randr::failure_means_unsupported(
    "failed to connect to display\n"));
  EXPECT_TRUE(platf::wlr_randr::failure_means_unsupported(
    "some prefix\nfailed to connect to display\n"));

  EXPECT_FALSE(platf::wlr_randr::failure_means_unsupported(""));
  EXPECT_FALSE(platf::wlr_randr::failure_means_unsupported("failed to apply configuration\n"));
  EXPECT_FALSE(platf::wlr_randr::failure_means_unsupported("unknown mode '1920x1080@61'\n"));
}

TEST(WlrRandr, MissingToolDoesNotClaimTheSession) {
  if (platf::wlr_randr::available()) {
    GTEST_SKIP() << "wlr-randr is installed; the discovery path is exercised by a live session instead";
  }

  // Without the tool there is no backend, and the probe must not arm a cooldown
  // for a compositor it never reached (the caller falls through to KScreen).
  EXPECT_FALSE(platf::wlr_randr::session_supports_wlr_randr());
}
