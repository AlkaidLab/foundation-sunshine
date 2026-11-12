/**
 * @file tests/unit/test_touch_plan.cpp
 * @brief Tests for the mapping of client touch events onto touchscreen contacts.
 */
#include <gtest/gtest.h>

#include "src/platform/linux/input/touch_plan.h"

namespace {
  using platf::touch::action_e;
  using platf::touch::kDefaultPressure;
  using platf::touch::kMaxContacts;

  /// A touch event with every field a client always sends, so a test only has to name the
  /// ones it cares about.
  platf::touch_input_t
  event(std::uint8_t type, float x = 0.5f, float y = 0.5f) {
    platf::touch_input_t touch {};
    touch.eventType = type;
    touch.pointerId = 1;
    touch.x = x;
    touch.y = y;
    touch.pressureOrDistance = 0.0f;
    touch.rotation = LI_ROT_UNKNOWN;
    return touch;
  }

  TEST(TouchPlan, HoverIsNotAContact) {
    // The client moving a finger over the screen without touching must not put a contact
    // down: Windows keeps hover in-range-but-not-contacting.
    const auto plan = platf::touch::plan_for(event(LI_TOUCH_EVENT_HOVER), false, 0);
    EXPECT_EQ(plan.action, action_e::ignore);
  }

  TEST(TouchPlan, HoverReleasesAContactWeHold) {
    // A hover for a pointer we already have down means the client stopped touching.
    const auto plan = platf::touch::plan_for(event(LI_TOUCH_EVENT_HOVER), true, 1);
    EXPECT_EQ(plan.action, action_e::release);
  }

  TEST(TouchPlan, DownAndMovePlaceAContact) {
    EXPECT_EQ(platf::touch::plan_for(event(LI_TOUCH_EVENT_DOWN), false, 0).action, action_e::place);
    EXPECT_EQ(platf::touch::plan_for(event(LI_TOUCH_EVENT_MOVE), true, 2).action, action_e::place);
    // A MOVE for a pointer we do not know yet is still a contact: the client is authoritative.
    EXPECT_EQ(platf::touch::plan_for(event(LI_TOUCH_EVENT_MOVE), false, 2).action, action_e::place);
  }

  TEST(TouchPlan, LiftVariantsRelease) {
    for (auto type : {LI_TOUCH_EVENT_UP, LI_TOUCH_EVENT_CANCEL, LI_TOUCH_EVENT_HOVER_LEAVE}) {
      EXPECT_EQ(platf::touch::plan_for(event(type), true, 1).action, action_e::release) << (int) type;
    }
  }

  TEST(TouchPlan, CancelAllReleasesEveryContact) {
    // Without this the host keeps tracking fingers the client dropped, and the next gesture
    // arrives as an extra finger.
    const auto plan = platf::touch::plan_for(event(LI_TOUCH_EVENT_CANCEL_ALL), false, 3);
    EXPECT_EQ(plan.action, action_e::release_all);
    EXPECT_FALSE(plan.place_after_release);
  }

  TEST(TouchPlan, CancelAllForAKnownPointerStillReleasesEverything) {
    const auto plan = platf::touch::plan_for(event(LI_TOUCH_EVENT_CANCEL_ALL), true, 2);
    EXPECT_EQ(plan.action, action_e::release_all);
    EXPECT_FALSE(plan.place_after_release);
  }

  TEST(TouchPlan, ButtonOnlyAndUnknownEventsDoNothing) {
    EXPECT_EQ(platf::touch::plan_for(event(LI_TOUCH_EVENT_BUTTON_ONLY), false, 0).action, action_e::ignore);
    EXPECT_EQ(platf::touch::plan_for(event(LI_TOUCH_EVENT_BUTTON_ONLY), true, 1).action, action_e::ignore);
    EXPECT_EQ(platf::touch::plan_for(event(0x42), true, 1).action, action_e::ignore);
    EXPECT_FALSE(platf::touch::known_event(0x42));
  }

  TEST(TouchPlan, KnowsEveryAnnouncedEvent) {
    for (auto type : {LI_TOUCH_EVENT_HOVER, LI_TOUCH_EVENT_DOWN, LI_TOUCH_EVENT_UP, LI_TOUCH_EVENT_MOVE,
             LI_TOUCH_EVENT_CANCEL, LI_TOUCH_EVENT_BUTTON_ONLY, LI_TOUCH_EVENT_HOVER_LEAVE, LI_TOUCH_EVENT_CANCEL_ALL}) {
      EXPECT_TRUE(platf::touch::known_event(type)) << (int) type;
    }
  }

  TEST(TouchPlan, UnknownRotationDoesNotClaimAnOrientation) {
    EXPECT_EQ(platf::touch::orientation_for(LI_ROT_UNKNOWN), 0);
  }

  TEST(TouchPlan, RotationMapsToTheDeviceRange) {
    EXPECT_EQ(platf::touch::orientation_for(0), 0);
    EXPECT_EQ(platf::touch::orientation_for(45), 45);
    EXPECT_EQ(platf::touch::orientation_for(90), 90);
    EXPECT_EQ(platf::touch::orientation_for(135), 45);
    EXPECT_EQ(platf::touch::orientation_for(180), 0);
    EXPECT_EQ(platf::touch::orientation_for(225), -45);
    EXPECT_EQ(platf::touch::orientation_for(270), -90);
    EXPECT_EQ(platf::touch::orientation_for(315), -45);
    EXPECT_EQ(platf::touch::orientation_for(350), -10);
    EXPECT_EQ(platf::touch::orientation_for(360), 0);
  }

  TEST(TouchPlan, RotationIsAlwaysInsideTheDeviceRange) {
    for (int rotation = 0; rotation < 360; ++rotation) {
      const auto orientation = platf::touch::orientation_for(rotation);
      EXPECT_GE(orientation, -90) << rotation;
      EXPECT_LE(orientation, 90) << rotation;
    }
  }

  TEST(TouchPlan, PlacementCarriesOrientationAndPressure) {
    auto touch = event(LI_TOUCH_EVENT_DOWN);
    touch.rotation = 30;
    touch.pressureOrDistance = 0.25f;

    const auto plan = platf::touch::plan_for(touch, false, 0);
    EXPECT_EQ(plan.action, action_e::place);
    EXPECT_EQ(plan.orientation, 30);
    EXPECT_FLOAT_EQ(plan.pressure, 0.25f);
  }

  TEST(TouchPlan, MissingPressureDoesNotReadAsZero) {
    // Windows substitutes 512/1024 for a client that reports nothing.
    EXPECT_FLOAT_EQ(platf::touch::pressure_for(0.0f), kDefaultPressure);
    EXPECT_FLOAT_EQ(platf::touch::pressure_for(0.8f), 0.8f);

    const auto plan = platf::touch::plan_for(event(LI_TOUCH_EVENT_DOWN), false, 0);
    EXPECT_FLOAT_EQ(plan.pressure, kDefaultPressure);
  }

  TEST(TouchPlan, PointerTableOverflowCancelsAndRestarts) {
    // Windows cancels every active touch when it runs out of pointer entries, then places
    // the new one.
    auto overflow = platf::touch::plan_for(event(LI_TOUCH_EVENT_DOWN), false, kMaxContacts);
    EXPECT_EQ(overflow.action, action_e::release_all);
    EXPECT_TRUE(overflow.place_after_release);
    EXPECT_FLOAT_EQ(overflow.pressure, kDefaultPressure);

    // One fewer contact and there is still a free slot.
    auto fits = platf::touch::plan_for(event(LI_TOUCH_EVENT_DOWN), false, kMaxContacts - 1);
    EXPECT_EQ(fits.action, action_e::place);
    EXPECT_FALSE(fits.place_after_release);

    // An existing contact is updated, never treated as a new one.
    auto existing = platf::touch::plan_for(event(LI_TOUCH_EVENT_MOVE), true, kMaxContacts);
    EXPECT_EQ(existing.action, action_e::place);
    EXPECT_FALSE(existing.place_after_release);
  }
}  // namespace
