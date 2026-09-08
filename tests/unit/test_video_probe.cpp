/**
 * @file tests/unit/test_video_probe.cpp
 * @brief Tests for temporary encoder-probe display selection.
 */
#include <src/video_probe.h>

#include <array>
#include <gtest/gtest.h>

TEST(VideoProbe, PreservesCaptureReadyConfiguredDisplay) {
  const std::array displays { std::string { R"(\\.\DISPLAY7)" }, std::string { R"(\\.\DISPLAY9)" } };
  EXPECT_EQ(video::select_encoder_probe_display(R"(\\.\DISPLAY9)", displays), R"(\\.\DISPLAY9)");
}

TEST(VideoProbe, MissingConfiguredDisplayUsesBackendAutoselection) {
  const std::array displays { std::string { R"(\\.\DISPLAY7)" }, std::string { R"(\\.\DISPLAY9)" } };
  EXPECT_TRUE(video::select_encoder_probe_display(R"(\\.\DISPLAY18)", displays).empty());
}

TEST(VideoProbe, EmptyConfiguredDisplayPreservesBackendAutoselection) {
  const std::array displays { std::string { R"(\\.\DISPLAY7)" }, std::string { R"(\\.\DISPLAY9)" } };
  EXPECT_TRUE(video::select_encoder_probe_display({}, displays).empty());
}

TEST(VideoProbe, EmptyCaptureReadyListUsesBackendAutoselection) {
  const std::array<std::string, 0> displays {};
  EXPECT_TRUE(video::select_encoder_probe_display(R"(\\.\DISPLAY18)", displays).empty());
}
