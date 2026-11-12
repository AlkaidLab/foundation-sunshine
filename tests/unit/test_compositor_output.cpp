/**
 * @file tests/unit/test_compositor_output.cpp
 * @brief Compositor output-enable dispatch (Linux VDD).
 *
 * Regression coverage for the desktop -> command mapping: the KDE branch used to
 * call back into the dispatcher itself, so creating a virtual display on Plasma
 * blew the stack (SIGSEGV in enable_output_via_compositor). The mapping is pure,
 * so every branch is pinned here without a compositor present.
 */
#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

#include "src/platform/linux/compositor_output.h"

using platf::compositor_output::enable_command;
using platf::compositor_output::niri_session;
using platf::compositor_output::tool_available;

namespace {
  constexpr const char *kConnector = "DP-2";
}  // namespace

TEST(CompositorOutput, KdeKeepsTheTestedKscreenCommand) {
  EXPECT_EQ(enable_command("KDE", kConnector, false, false, false, false),
            "kscreen-doctor output.DP-2.enable");
  // kscreen-doctor is what was tested on Plasma, so a session that only says
  // "plasma" gets the same command even when every other tool is present.
  EXPECT_EQ(enable_command("plasma", kConnector, true, true, true, true),
            "kscreen-doctor output.DP-2.enable");
}

TEST(CompositorOutput, NiriUsesItsOwnIpc) {
  EXPECT_EQ(enable_command("niri", kConnector, true, true, true, true),
            "niri msg output DP-2 on");
}

TEST(CompositorOutput, WlrRandrCoversOtherWaylandCompositors) {
  EXPECT_EQ(enable_command("sway", kConnector, false, true, false, false),
            "wlr-randr --output DP-2 --on");
}

TEST(CompositorOutput, X11FallsBackToXrandr) {
  EXPECT_EQ(enable_command("XFCE", kConnector, false, false, true, true),
            "xrandr --output DP-2 --auto");
}

TEST(CompositorOutput, UnknownSessionHasNoCommand) {
  // An X11 session without xrandr, and a Wayland session with no tool at all:
  // both must stay silent rather than guess a command.
  EXPECT_TRUE(enable_command("XFCE", kConnector, false, false, true, false).empty());
  EXPECT_TRUE(enable_command("", kConnector, false, false, false, false).empty());
}

TEST(CompositorOutput, NiriSessionProbeKeysOffTheSocket) {
  ::unsetenv("NIRI_SOCKET");
  EXPECT_FALSE(niri_session());

  ::setenv("NIRI_SOCKET", "/run/user/1000/niri.wayland-1.8.sock", 1);
  EXPECT_TRUE(niri_session());

  // An empty variable is what a stripped environment looks like, not a session.
  ::setenv("NIRI_SOCKET", "", 1);
  EXPECT_FALSE(niri_session());

  ::unsetenv("NIRI_SOCKET");
}

TEST(CompositorOutput, ToolProbeLooksAtTheBinDirectories) {
  EXPECT_TRUE(tool_available("sh"));
  EXPECT_FALSE(tool_available("sunshine-no-such-tool-for-tests"));
}
