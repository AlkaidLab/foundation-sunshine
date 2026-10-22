/**
 * @file tests/unit/test_display_backend.cpp
 * @brief Tests for the display backend precedence.
 *
 * The order in which the display backends are asked is part of the contract,
 * not an implementation detail: XWayland answers RandR reads inside every
 * Wayland session, so a wlroots or Mutter session that asks RandR first would
 * be served by a read-only mirror of its real outputs. These tests pin that
 * order as pure logic, which is the only way to cover it here -- one machine
 * cannot host Plasma, GNOME, niri and sway at once.
 */
#include <gtest/gtest.h>

#include <vector>

#include "src/platform/linux/display_backend.h"

namespace {
  using platf::display_backend_e;

  /**
   * @brief Run the selector with a fixed set of claims and record the order.
   * @param claims Which backends accept the session.
   * @param asked Receives every backend the selector consulted, in order.
   * @return The selected backend.
   */
  display_backend_e
  select_with(bool claims[4], std::vector<display_backend_e> *asked = nullptr) {
    return platf::select_display_backend([&](display_backend_e backend) {
      if (asked != nullptr) {
        asked->push_back(backend);
      }

      switch (backend) {
        case display_backend_e::niri:
          return claims[0];
        case display_backend_e::mutter:
          return claims[1];
        case display_backend_e::wlr_randr:
          return claims[2];
        case display_backend_e::xrandr:
          return claims[3];
        case display_backend_e::kscreen:
          return false;
      }
      return false;
    });
  }

  std::string
  to_string(const std::vector<display_backend_e> &backends) {
    std::string result;
    for (const auto backend : backends) {
      if (!result.empty()) {
        result += ",";
      }
      switch (backend) {
        case display_backend_e::niri: result += "niri"; break;
        case display_backend_e::mutter: result += "mutter"; break;
        case display_backend_e::wlr_randr: result += "wlr_randr"; break;
        case display_backend_e::xrandr: result += "xrandr"; break;
        case display_backend_e::kscreen: result += "kscreen"; break;
      }
    }
    return result;
  }
}  // namespace

TEST(DisplayBackend, KScreenIsTheFloorWhenNothingClaimsTheSession) {
  bool claims[4] = { false, false, false, false };
  EXPECT_EQ(select_with(claims), display_backend_e::kscreen);
}

TEST(DisplayBackend, XrandrIsTheLastResortBeforeKScreen) {
  bool claims[4] = { false, false, false, true };
  EXPECT_EQ(select_with(claims), display_backend_e::xrandr);
}

/**
 * The trap this order exists for: XWayland answers RandR queries in a wlroots
 * session, so both probes accept and the compositor's own output manager has to
 * win.
 */
TEST(DisplayBackend, WlrootsBeatsTheXWaylandMirror) {
  bool claims[4] = { false, false, true, true };
  EXPECT_EQ(select_with(claims), display_backend_e::wlr_randr);
}

TEST(DisplayBackend, MutterBeatsWlrootsAndXrandr) {
  bool claims[4] = { false, true, true, true };
  EXPECT_EQ(select_with(claims), display_backend_e::mutter);
}

TEST(DisplayBackend, NiriBeatsEveryOtherBackend) {
  bool claims[4] = { true, true, true, true };
  EXPECT_EQ(select_with(claims), display_backend_e::niri);
}

TEST(DisplayBackend, EachBackendWinsOnItsOwn) {
  bool niri[4] = { true, false, false, false };
  bool mutter[4] = { false, true, false, false };
  bool wlroots[4] = { false, false, true, false };

  EXPECT_EQ(select_with(niri), display_backend_e::niri);
  EXPECT_EQ(select_with(mutter), display_backend_e::mutter);
  EXPECT_EQ(select_with(wlroots), display_backend_e::wlr_randr);
}

/**
 * Probing is not free -- the wlroots probe may run the helper tool -- so the
 * chain has to stop at the winner instead of asking everybody and picking.
 */
TEST(DisplayBackend, ProbingStopsAtTheWinner) {
  bool all[4] = { true, true, true, true };
  std::vector<display_backend_e> asked;
  select_with(all, &asked);
  EXPECT_EQ(to_string(asked), "niri") << "a niri session must not probe anything else";

  bool xrandr_only[4] = { false, false, false, true };
  asked.clear();
  select_with(xrandr_only, &asked);
  EXPECT_EQ(to_string(asked), "niri,mutter,wlr_randr,xrandr")
    << "RandR is only reached after every compositor API declined";

  bool none[4] = { false, false, false, false };
  asked.clear();
  select_with(none, &asked);
  EXPECT_EQ(to_string(asked), "niri,mutter,wlr_randr,xrandr")
    << "KScreen has no probe of its own: it is what remains after the chain declined";
}
