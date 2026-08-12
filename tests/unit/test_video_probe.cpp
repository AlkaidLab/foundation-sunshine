/**
 * @file tests/unit/test_video_probe.cpp
 * @brief Tests for temporary encoder-probe display selection.
 */
#include <src/video_probe.h>

#include <array>
#include <gtest/gtest.h>

TEST(VideoProbe, PreservesCaptureReadyConfiguredDisplay) {
  const std::array displays { std::string { R"(\\.\DISPLAY7)" }, std::string { R"(\\.\DISPLAY9)" } };
  const auto selection = video::select_encoder_probe_display(
    R"(\\.\DISPLAY9)",
    displays,
    video::probe_display_policy_e::vdd_compatible);
  EXPECT_EQ(selection.selection, video::probe_display_selection_e::exact);
  EXPECT_EQ(selection.display_name, R"(\\.\DISPLAY9)");
}

TEST(VideoProbe, MissingConfiguredDisplayUsesBackendAutoselection) {
  const std::array displays { std::string { R"(\\.\DISPLAY7)" }, std::string { R"(\\.\DISPLAY9)" } };
  const auto selection = video::select_encoder_probe_display(
    R"(\\.\DISPLAY18)",
    displays,
    video::probe_display_policy_e::vdd_compatible);
  EXPECT_EQ(selection.selection, video::probe_display_selection_e::backend_autoselect);
  EXPECT_TRUE(selection.display_name.empty());
}

TEST(VideoProbe, EmptyConfiguredDisplayPreservesBackendAutoselection) {
  const std::array displays { std::string { R"(\\.\DISPLAY7)" }, std::string { R"(\\.\DISPLAY9)" } };
  const auto selection = video::select_encoder_probe_display(
    {},
    displays,
    video::probe_display_policy_e::vdd_compatible);
  EXPECT_EQ(selection.selection, video::probe_display_selection_e::backend_autoselect);
  EXPECT_TRUE(selection.display_name.empty());
}

TEST(VideoProbe, EmptyCaptureReadyListUsesBackendAutoselection) {
  const std::array<std::string, 0> displays {};
  const auto selection = video::select_encoder_probe_display(
    R"(\\.\DISPLAY18)",
    displays,
    video::probe_display_policy_e::vdd_compatible);
  EXPECT_EQ(selection.selection, video::probe_display_selection_e::backend_autoselect);
  EXPECT_TRUE(selection.display_name.empty());
}

TEST(VideoProbe, ExactTargetNeverFallsBackToAnotherDisplay) {
  const std::array displays { std::string { R"(\\.\DISPLAY7)" }, std::string { R"(\\.\DISPLAY9)" } };
  const auto selection = video::select_encoder_probe_display(
    R"(\\.\DISPLAY18)",
    displays,
    video::probe_display_policy_e::exact);
  EXPECT_EQ(selection.selection, video::probe_display_selection_e::unavailable);
  EXPECT_TRUE(selection.display_name.empty());
}

TEST(VideoProbe, ExactTargetRejectsAnUnresolvedDevice) {
  const std::array displays { std::string { R"(\\.\DISPLAY7)" } };
  const auto selection = video::select_encoder_probe_display(
    {},
    displays,
    video::probe_display_policy_e::exact);
  EXPECT_EQ(selection.selection, video::probe_display_selection_e::unavailable);
}

TEST(VideoProbe, VddCompatibilityCanUseBackendAutoselection) {
  const std::array displays { std::string { R"(\\.\DISPLAY7)" } };
  const auto selection = video::select_encoder_probe_display(
    {},
    displays,
    video::probe_display_policy_e::vdd_compatible);
  EXPECT_EQ(selection.selection, video::probe_display_selection_e::backend_autoselect);
  EXPECT_TRUE(selection.display_name.empty());
}

TEST(VideoProbe, ExactCaptureReadyTargetIsPreserved) {
  const std::array displays { std::string { R"(\\.\DISPLAY7)" }, std::string { R"(\\.\DISPLAY9)" } };
  const auto selection = video::select_encoder_probe_display(
    R"(\\.\DISPLAY9)",
    displays,
    video::probe_display_policy_e::exact);
  EXPECT_EQ(selection.selection, video::probe_display_selection_e::exact);
  EXPECT_EQ(selection.display_name, R"(\\.\DISPLAY9)");
}
