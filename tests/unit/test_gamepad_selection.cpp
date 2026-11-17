/** @file tests/unit/test_gamepad_selection.cpp
 *  @brief Session and late-arrival virtual controller selection tests.
 */
#include "src/platform/gamepad_selection.h"

#include <iterator>

#include <gtest/gtest.h>

TEST(GamepadSelection, DeclaredDualSenseAppliesToLateAllocation) {
  EXPECT_EQ(platf::gamepad_selection::select(
              platf::gamepad_selection::automatic, "ds5", true, true, false),
    platf::gamepad_selection::dualsense);
}

TEST(GamepadSelection, DualSenseArrivalCapabilitySelectsNativePath) {
  EXPECT_EQ(platf::gamepad_selection::select(
              platf::gamepad_selection::dualshock4, {}, true,
              platf::gamepad_selection::playstation_arrival_type,
              platf::gamepad_selection::dualsense_arrival_capability),
    platf::gamepad_selection::dualsense);
}

TEST(GamepadSelection, ObservedLegacyArrivalRemainsDualShock4) {
  constexpr std::uint8_t type = 2;
  constexpr std::uint16_t capabilities = 251;
  constexpr std::uint32_t supported_buttons = 0x30f7ff;
  (void) supported_buttons;
  EXPECT_EQ(platf::gamepad_selection::select(
              platf::gamepad_selection::dualshock4, {}, true, type, capabilities),
    platf::gamepad_selection::dualshock4);
}

TEST(GamepadSelection, ObservedArrivalWithDualSenseIdentitySelectsNativePath) {
  constexpr std::uint8_t type = 2;
  constexpr std::uint16_t capabilities =
    251 | platf::gamepad_selection::dualsense_arrival_capability;
  constexpr std::uint32_t supported_buttons = 0x30f7ff;
  (void) supported_buttons;
  EXPECT_TRUE(platf::gamepad_selection::is_dualsense_arrival(type, capabilities));
  EXPECT_EQ(platf::gamepad_selection::select(
              platf::gamepad_selection::dualshock4, {}, true, type, capabilities),
    platf::gamepad_selection::dualsense);
}

TEST(GamepadSelection, LegacyPlayStationArrivalKeepsExistingFallback) {
  EXPECT_EQ(platf::gamepad_selection::select(
              platf::gamepad_selection::dualshock4, {}, true,
              platf::gamepad_selection::playstation_arrival_type, 0),
    platf::gamepad_selection::dualshock4);
}

TEST(GamepadSelection, HostPolicyWinsWhenClientOverrideIsDisabled) {
  EXPECT_EQ(platf::gamepad_selection::select(
              platf::gamepad_selection::dualshock4, "ds5", false, true, true),
    platf::gamepad_selection::dualshock4);
}

TEST(GamepadSelection, AutomaticSessionAdvertisesLateDualSenseCapability) {
  EXPECT_TRUE(platf::gamepad_selection::advertise_ds5_capability(
    true, platf::gamepad_selection::automatic, {}, true));
  EXPECT_FALSE(platf::gamepad_selection::advertise_ds5_capability(
    true, platf::gamepad_selection::dualshock4, {}, true));
  EXPECT_FALSE(platf::gamepad_selection::advertise_ds5_capability(
    true, platf::gamepad_selection::automatic, {}, false));
}

TEST(GamepadSelection, ExplicitAutomaticDoesNotPromoteLatePad) {
  EXPECT_EQ(platf::gamepad_selection::select(
              platf::gamepad_selection::dualshock4, "auto", true,
              platf::gamepad_selection::playstation_arrival_type, 0),
    platf::gamepad_selection::automatic);
}

TEST(GamepadSelection, DualSenseIdentityNeedsPlayStationTypeAndCapability) {
  const auto ps = platf::gamepad_selection::playstation_arrival_type;
  EXPECT_TRUE(platf::gamepad_selection::identifies_dualsense(ps, platf::gamepad_selection::dualsense_arrival_capability));
  EXPECT_TRUE(platf::gamepad_selection::identifies_dualsense(ps, platf::gamepad_selection::dualsense_pcm_capability));
  EXPECT_TRUE(platf::gamepad_selection::identifies_dualsense(ps,
    platf::gamepad_selection::dualsense_pcm_capability | platf::gamepad_selection::dualsense_arrival_capability));
  // A PlayStation pad without either capability may be a DualSense; the client does not say.
  EXPECT_FALSE(platf::gamepad_selection::identifies_dualsense(ps, 251));
  EXPECT_FALSE(platf::gamepad_selection::identifies_dualsense(ps, 0));
  // The capabilities of another pad type never identify a DualSense.
  EXPECT_FALSE(platf::gamepad_selection::identifies_dualsense(1, platf::gamepad_selection::dualsense_arrival_capability));
}

TEST(GamepadSelection, SummarizeAnswersWithTheFirstPlayStationPad) {
  using platf::gamepad_selection::pad_facts_t;
  constexpr pad_facts_t xbox { .allocated = true, .playstation_arrival = false, .dualsense_identity = false,
    .virtual_type = platf::gamepad_selection::pad_type_xbox360 };
  constexpr pad_facts_t legacy_ps { .allocated = true, .playstation_arrival = true, .dualsense_identity = false,
    .virtual_type = platf::gamepad_selection::pad_type_dualshock4 };
  constexpr pad_facts_t pads[] { xbox, legacy_ps };
  const auto summary = platf::gamepad_selection::summarize_pads(pads, std::size(pads));
  EXPECT_TRUE(summary.allocated);
  EXPECT_TRUE(summary.playstation_arrival);
  EXPECT_FALSE(summary.dualsense_identity);
  EXPECT_EQ(summary.virtual_type, platf::gamepad_selection::pad_type_dualshock4);
}

TEST(GamepadSelection, SummarizeSkipsFreeSlotsAndUndeclaredPads) {
  using platf::gamepad_selection::pad_facts_t;
  constexpr pad_facts_t free_slot {};
  constexpr pad_facts_t multi_controller_pad { .allocated = true, .playstation_arrival = false,
    .dualsense_identity = false, .virtual_type = platf::gamepad_selection::pad_type_dualshock4 };
  constexpr pad_facts_t pads[] { free_slot, multi_controller_pad };
  // A pad created by a multi-controller packet carries no arrival metadata, so it never
  // answers for the DS4/DualSense distinction even when it runs as a DS4.
  const auto summary = platf::gamepad_selection::summarize_pads(pads, std::size(pads));
  EXPECT_FALSE(summary.allocated);
  EXPECT_EQ(summary.virtual_type, std::string_view {});
}
