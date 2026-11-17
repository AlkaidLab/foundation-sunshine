/**
 * @file tests/unit/test_audio_sink_plan.cpp
 * @brief Which virtual device carries a muted stream, which layout it is asked for and which wave format is tried first.
 *        No audio API is touched.
 */
#include <gtest/gtest.h>

#include "src/audio_sink_plan.h"

namespace {
  using namespace audio_sink_plan;

  // the ids Windows gives, and the names a Chinese Windows shows: none of them may change the choice
  constexpr auto cable_id = "{0.0.0.00000000}.{172f61d5-5aff-4b19-9233-e02c0003996f}";
  constexpr auto cable_name = "CABLE Input (VB-Audio Virtual Cable)";
  constexpr auto steam_name = "扬声器 (Steam Streaming Speakers)";
}  // namespace

TEST(AudioSinkPlanTarget, NothingConfiguredUsesSteam) {
  EXPECT_EQ(choose_virtual_target("", "", false), virtual_target_e::steam_streaming);
}

TEST(AudioSinkPlanTarget, ConfiguredVirtualSinkWins) {
  EXPECT_EQ(choose_virtual_target(cable_id, "", false), virtual_target_e::virtual_sink);
  EXPECT_EQ(choose_virtual_target(steam_name, cable_name, true), virtual_target_e::virtual_sink);
}

TEST(AudioSinkPlanTarget, CapturedVirtualDeviceCarriesTheStream) {
  // the owner's case: VB-CABLE chosen as the device to capture, nothing as the virtual sink: the sound has to go to VB-CABLE
  EXPECT_EQ(choose_virtual_target("", cable_id, true), virtual_target_e::audio_sink);
  EXPECT_EQ(choose_virtual_target("", cable_name, true), virtual_target_e::audio_sink);
}

TEST(AudioSinkPlanTarget, CapturedPhysicalDeviceDoesNotChangeTheVirtualDevice) {
  EXPECT_EQ(choose_virtual_target("", "Speakers (Realtek(R) Audio)", false), virtual_target_e::steam_streaming);
  // a captured value that matches no device at all is not a virtual device either
  EXPECT_EQ(choose_virtual_target("", "耳机 (USB Audio)", false), virtual_target_e::steam_streaming);
}

TEST(AudioSinkPlanLayout, LadderGoesDownToStereo) {
  EXPECT_EQ(layout_ladder(12), (std::vector<int> { 12, 8, 6, 2 }));
  EXPECT_EQ(layout_ladder(8), (std::vector<int> { 8, 6, 2 }));
  EXPECT_EQ(layout_ladder(6), (std::vector<int> { 6, 2 }));
  EXPECT_EQ(layout_ladder(2), (std::vector<int> { 2 }));
}

TEST(AudioSinkPlanLayout, UnusualCountsStillEndWithStereo) {
  EXPECT_EQ(layout_ladder(1), (std::vector<int> { 2 }));
  EXPECT_EQ(layout_ladder(0), (std::vector<int> { 2 }));
  EXPECT_EQ(layout_ladder(16), (std::vector<int> { 12, 8, 6, 2 }));
  EXPECT_EQ(layout_ladder(7), (std::vector<int> { 6, 2 }));
}

TEST(AudioSinkPlanLayout, FullSupportKeepsTheRequestedLayout) {
  EXPECT_EQ(pick_layout(8, [](int) { return true; }), 8);
  EXPECT_EQ(pick_layout(12, [](int) { return true; }), 12);
}

TEST(AudioSinkPlanLayout, DeviceWithoutSevenOneStepsDownToFiveOne) {
  // a virtual device that has 5.1 but no 7.1 or 7.1.4
  const auto only_up_to_six = [](int channels) { return channels <= 6; };
  EXPECT_EQ(pick_layout(12, only_up_to_six), 6);
  EXPECT_EQ(pick_layout(8, only_up_to_six), 6);
  EXPECT_EQ(pick_layout(6, only_up_to_six), 6);
}

TEST(AudioSinkPlanLayout, StereoOnlyDeviceGetsStereo) {
  const auto nothing_wider = [](int channels) { return channels == 2; };
  EXPECT_EQ(pick_layout(12, nothing_wider), 2);
  EXPECT_EQ(pick_layout(6, nothing_wider), 2);
}

TEST(AudioSinkPlanLayout, NoAnswerAtAllFallsBackToStereo) {
  EXPECT_EQ(pick_layout(8, [](int) { return false; }), 2);
  EXPECT_EQ(pick_layout(8, nullptr), 2);
}

TEST(AudioSinkPlanFormats, WantedDepthComesFirstThenTheNearest) {
  // the 5.1 list of the host: f32 s32 s24in32 s24 s16 (valid bits 32 32 24 24 16)
  const std::vector<int> bits { 32, 32, 24, 24, 16 };
  EXPECT_EQ(format_try_order(bits, 24), (std::vector<std::size_t> { 2, 3, 0, 1, 4 }));
  EXPECT_EQ(format_try_order(bits, 32), (std::vector<std::size_t> { 0, 1, 2, 3, 4 }));
  EXPECT_EQ(format_try_order(bits, 16), (std::vector<std::size_t> { 4, 2, 3, 0, 1 }));
}

TEST(AudioSinkPlanFormats, DeviceWithoutTheWantedDepthStillGetsTheOthers) {
  // the default device mixes in 32 bit, VB-CABLE has formats of 24 and 16 bit only: the old rule skipped all of them
  const std::vector<int> bits { 24, 24, 16, 32, 32 };  // the stereo list: s24in32 s24 s16 f32 s32
  const auto order = format_try_order(bits, 32);
  ASSERT_EQ(order.size(), bits.size());
  EXPECT_EQ(order[0], 3u);
  EXPECT_EQ(order[1], 4u);
  // after the 32-bit ones the 24-bit ones, then 16
  EXPECT_EQ(order[2], 0u);
  EXPECT_EQ(order[3], 1u);
  EXPECT_EQ(order[4], 2u);
}

TEST(AudioSinkPlanFormats, NoPreferenceKeepsTheListOrder) {
  const std::vector<int> bits { 24, 24, 16, 32, 32 };
  EXPECT_EQ(format_try_order(bits, 0), (std::vector<std::size_t> { 0, 1, 2, 3, 4 }));
  EXPECT_TRUE(format_try_order({}, 24).empty());
}
