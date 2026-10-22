#include <gtest/gtest.h>

#include "src/platform/linux/xrandr_display.h"

namespace {

  using namespace platf::xrandr;

  platf::xrandr::mode_t
  make_mode(int width, int height, double refresh, bool current = false) {
    platf::xrandr::mode_t mode;
    mode.width = width;
    mode.height = height;
    mode.refresh = refresh;
    mode.current = current;
    mode.id = std::to_string(width) + "x" + std::to_string(height) + "@" + std::to_string(refresh);
    return mode;
  }

  output_t
  make_output(std::string name, int x, int y) {
    output_t output;
    output.name = std::move(name);
    output.connected = true;
    output.enabled = true;
    output.x = x;
    output.y = y;
    auto mode = make_mode(1920, 1080, 60.0);
    output.width = mode.width;
    output.height = mode.height;
    output.refresh = mode.refresh;
    output.mode_id = mode.id;
    output.modes.push_back(mode);
    return output;
  }

  /// Two 1920x1080 outputs, one to the right of the other.
  state_t
  side_by_side() {
    state_t state;
    state.outputs.push_back(make_output("DP-1", 0, 0));
    state.outputs.push_back(make_output("DP-2", 1920, 0));
    return state;
  }

}  // namespace

TEST(XrandrPlan, OneEnabledOutputBecomesOneCrtc) {
  state_t state;
  state.outputs.push_back(make_output("DP-1", 0, 0));

  plan_t plan;
  std::string error;
  ASSERT_TRUE(plan_for(state, plan, error)) << error;

  ASSERT_EQ(plan.crtcs.size(), 1u);
  EXPECT_EQ(plan.crtcs.front().outputs, std::vector<std::string> { "DP-1" });
  EXPECT_EQ(plan.crtcs.front().mode_id, state.outputs.front().mode_id);
  EXPECT_EQ(plan.screen_width, 1920);
  EXPECT_EQ(plan.screen_height, 1080);
}

TEST(XrandrPlan, DisabledOutputGetsNoCrtc) {
  auto state = side_by_side();
  state.outputs[1].enabled = false;

  plan_t plan;
  std::string error;
  ASSERT_TRUE(plan_for(state, plan, error)) << error;

  ASSERT_EQ(plan.crtcs.size(), 1u);
  EXPECT_EQ(plan.crtcs.front().outputs, std::vector<std::string> { "DP-1" });
  // The screen only has to cover what is actually scanned.
  EXPECT_EQ(plan.screen_width, 1920);
}

TEST(XrandrPlan, NothingEnabledStillYieldsAScreen) {
  auto state = side_by_side();
  for (auto &output : state.outputs) {
    output.enabled = false;
  }

  plan_t plan;
  std::string error;
  ASSERT_TRUE(plan_for(state, plan, error)) << error;

  EXPECT_TRUE(plan.crtcs.empty());
  // RandR rejects a zero-sized screen, so the plan never asks for one.
  EXPECT_EQ(plan.screen_width, 1);
  EXPECT_EQ(plan.screen_height, 1);
}

TEST(XrandrPlan, TwoOutputsSideBySideStaySeparate) {
  const auto state = side_by_side();

  plan_t plan;
  std::string error;
  ASSERT_TRUE(plan_for(state, plan, error)) << error;

  ASSERT_EQ(plan.crtcs.size(), 2u);
  EXPECT_EQ(plan.screen_width, 3840);
  EXPECT_EQ(plan.screen_height, 1080);
}

TEST(XrandrPlan, MirroringSharesOneCrtc) {
  auto state = side_by_side();
  // DP-2 mirrors DP-1: same crtc, same timing, one rectangle on the desktop.
  state.outputs[1].x = 0;
  state.outputs[1].y = 0;
  state.outputs[1].mirror_of = "DP-1";

  plan_t plan;
  std::string error;
  ASSERT_TRUE(plan_for(state, plan, error)) << error;

  ASSERT_EQ(plan.crtcs.size(), 1u);
  EXPECT_EQ(plan.crtcs.front().outputs, (std::vector<std::string> { "DP-1", "DP-2" }));
  // Mirroring does not double the desktop.
  EXPECT_EQ(plan.screen_width, 1920);
}

TEST(XrandrPlan, MirrorMemberWithADifferentSizeIsRefused) {
  auto state = side_by_side();
  state.outputs[1].mirror_of = "DP-1";
  state.outputs[1].width = 1280;
  state.outputs[1].height = 1024;
  state.outputs[1].modes = { make_mode(1280, 1024, 60.0, true) };
  state.outputs[1].mode_id = state.outputs[1].modes.front().id;

  plan_t plan;
  std::string error;
  EXPECT_FALSE(plan_for(state, plan, error));
  EXPECT_NE(error.find("mirrors DP-1"), std::string::npos) << error;
}

TEST(XrandrPlan, MirroringAnUnknownOutputIsRefused) {
  auto state = side_by_side();
  state.outputs[1].mirror_of = "HDMI-9";

  plan_t plan;
  std::string error;
  EXPECT_FALSE(plan_for(state, plan, error));
  EXPECT_NE(error.find("unknown output"), std::string::npos) << error;
}

TEST(XrandrPlan, MirroringADisabledOutputIsRefused) {
  auto state = side_by_side();
  state.outputs[0].enabled = false;
  state.outputs[1].mirror_of = "DP-1";

  plan_t plan;
  std::string error;
  EXPECT_FALSE(plan_for(state, plan, error));
  EXPECT_NE(error.find("disabled output"), std::string::npos) << error;
}

TEST(XrandrPlan, PrimaryFollowsTheOutputFlag) {
  auto state = side_by_side();
  state.outputs[1].primary = true;

  plan_t plan;
  std::string error;
  ASSERT_TRUE(plan_for(state, plan, error)) << error;
  EXPECT_EQ(plan.primary, "DP-2");
}

TEST(XrandrPlan, ConflictingPrimariesAreRefused) {
  auto state = side_by_side();
  state.outputs[0].primary = true;
  state.outputs[1].primary = true;

  plan_t plan;
  std::string error;
  EXPECT_FALSE(plan_for(state, plan, error));
  EXPECT_NE(error.find("primary"), std::string::npos) << error;
}

TEST(XrandrPlan, EnabledOutputWithoutAModeIsRefused) {
  state_t state;
  auto output = make_output("DP-1", 0, 0);
  output.modes.clear();
  output.mode_id.clear();
  state.outputs.push_back(output);

  plan_t plan;
  std::string error;
  EXPECT_FALSE(plan_for(state, plan, error));
  EXPECT_NE(error.find("no usable mode"), std::string::npos) << error;
}

TEST(XrandrPlan, ModeIdIsHonouredWhenItIsNotTheCurrentOne) {
  state_t state;
  auto output = make_output("DP-1", 0, 0);
  auto smaller = make_mode(1280, 720, 59.94);
  output.modes.push_back(smaller);
  // The caller asked for the smaller mode but nothing marked it current: the
  // plan has to take the requested id rather than fall back to the live one.
  output.mode_id = smaller.id;
  output.width = smaller.width;
  output.height = smaller.height;
  state.outputs.push_back(output);

  plan_t plan;
  std::string error;
  ASSERT_TRUE(plan_for(state, plan, error)) << error;
  ASSERT_EQ(plan.crtcs.size(), 1u);
  EXPECT_EQ(plan.crtcs.front().mode_id, smaller.id);
  EXPECT_EQ(plan.screen_width, 1280);
}

TEST(XrandrPlan, RotationAndReflectionAreCarriedIntoTheCrtc) {
  state_t state;
  auto output = make_output("DP-1", 0, 0);
  output.rotation = 1;
  output.reflect_x = true;
  state.outputs.push_back(output);

  plan_t plan;
  std::string error;
  ASSERT_TRUE(plan_for(state, plan, error)) << error;
  ASSERT_EQ(plan.crtcs.size(), 1u);
  EXPECT_EQ(plan.crtcs.front().rotation, 1);
  EXPECT_TRUE(plan.crtcs.front().reflect_x);
  EXPECT_FALSE(plan.crtcs.front().reflect_y);
}

TEST(XrandrRefresh, FollowsTheTimings) {
  // 1920x1080 at 60 Hz is a 148.5 MHz dot clock over 2200x1125 totals.
  EXPECT_NEAR(refresh_from_timings(148'500'000, 2200, 1125), 60.0, 0.01);
  // Degenerate timings must not divide by zero.
  EXPECT_DOUBLE_EQ(refresh_from_timings(148'500'000, 0, 1125), 0.0);
  EXPECT_DOUBLE_EQ(refresh_from_timings(0, 2200, 1125), 0.0);
}

TEST(XrandrModes, MatchingModeAcceptsAFractionalSlack) {
  output_t output;
  output.modes.push_back(make_mode(1920, 1080, 59.96));
  output.modes.push_back(make_mode(1920, 1080, 60.0));

  // Reported timings rarely match a nameplate rate exactly.
  const auto *matched = matching_mode(output, 1920, 1080, 59.95);
  ASSERT_NE(matched, nullptr);
  EXPECT_DOUBLE_EQ(matched->refresh, 59.96);

  EXPECT_EQ(matching_mode(output, 1280, 720, 60.0), nullptr);
}

TEST(XrandrModes, CurrentModeIsTheEnabledOne) {
  output_t output;
  output.modes.push_back(make_mode(1920, 1080, 60.0, false));
  output.modes.push_back(make_mode(1280, 720, 60.0, true));
  output.mode_id = output.modes.back().id;

  const auto *current = current_mode(output);
  ASSERT_NE(current, nullptr);
  EXPECT_EQ(current->width, 1280);

  // A disabled output has no current mode, however many it advertises.
  output.mode_id.clear();
  EXPECT_EQ(current_mode(output), nullptr);
}

// ── live server ─────────────────────────────────────────────────────────────
// These read the real X display. They cannot assert on the layout (that is the
// machine's business), only that the query agrees with what RandR says.

TEST(XrandrLive, QueryMatchesTheServerWhenOneIsReachable) {
  if (!session_supports_xrandr()) {
    GTEST_SKIP() << "no X display with RandR 1.2 in this environment";
  }

  const auto state = query_state();
  ASSERT_TRUE(state.has_value());
  ASSERT_FALSE(state->outputs.empty());

  for (const auto &output : state->outputs) {
    ASSERT_FALSE(output.name.empty());
    if (!output.enabled) {
      continue;
    }
    // An enabled output must have a mode we can name, and the rectangle it
    // scans has to match one of them.
    const auto *mode = current_mode(output);
    ASSERT_NE(mode, nullptr) << output.name;
    EXPECT_EQ(mode->width, output.width) << output.name;
    EXPECT_EQ(mode->height, output.height) << output.name;
  }
}

TEST(XrandrLive, PlanForTheLiveStateIsSelfConsistent) {
  if (!session_supports_xrandr()) {
    GTEST_SKIP() << "no X display with RandR 1.2 in this environment";
  }

  const auto state = query_state();
  ASSERT_TRUE(state.has_value());

  plan_t plan;
  std::string error;
  ASSERT_TRUE(plan_for(*state, plan, error)) << error;

  // Every enabled, connected output is either the head of a group or a member
  // of one, never both and never missing.
  std::vector<std::string> planned;
  for (const auto &request : plan.crtcs) {
    ASSERT_FALSE(request.outputs.empty());
    for (const auto &name : request.outputs) {
      planned.push_back(name);
    }
  }
  for (const auto &output : state->outputs) {
    const bool listed = std::find(planned.begin(), planned.end(), output.name) != planned.end();
    EXPECT_EQ(listed, output.enabled && output.connected) << output.name;
  }

  EXPECT_GE(plan.screen_width, 1);
  EXPECT_GE(plan.screen_height, 1);
  // Asking for what is already there must not be refused on planning grounds.
  EXPECT_LE(plan.screen_width, 32767);
}

TEST(XrandrLive, ApplyingTheCurrentStateIsHonouredOrCleanlyRefused) {
  if (!session_supports_xrandr()) {
    GTEST_SKIP() << "no X display with RandR 1.2 in this environment";
  }

  const auto before = query_state();
  ASSERT_TRUE(before.has_value());

  // Writing back what the server already shows must be safe on any server, and
  // it is the only write the test suite can attempt without changing the user's
  // desktop. XWayland in particular refuses modesets; that refusal has to be
  // reported as an error rather than silently ignored.
  std::string error;
  const bool applied = apply(*before, error);
  if (!applied) {
    EXPECT_FALSE(error.empty()) << "a refused write must say why";
  }

  const auto after = query_state();
  ASSERT_TRUE(after.has_value());
  EXPECT_EQ(after->outputs.size(), before->outputs.size());
  for (const auto &output : after->outputs) {
    // Whatever the server decided, every enabled output still has to have a
    // timing we can name -- a half-applied configuration would show up here.
    if (output.enabled) {
      EXPECT_NE(current_mode(output), nullptr) << output.name;
    }
  }
}
