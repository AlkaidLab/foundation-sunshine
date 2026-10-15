/** @file test_config_overrides.cpp
 * Command-line policy must survive saved-setting reloads with either map type.
 */
#include "src/config.h"
#include <gtest/gtest.h>

TEST(ConfigOverrides, ExplicitOptOutSurvivesMissingAndChangedDiskSettings) {
  const std::unordered_map<std::string, std::string> cli { { "nvenc_manage_driver_settings", "disabled" } };
  auto startup = std::unordered_map<std::string, std::string> {};
  config::apply_command_line_overrides(startup, cli);
  EXPECT_EQ(startup.at("nvenc_manage_driver_settings"), "disabled");
  auto reloaded = std::map<std::string, std::string> { { "nvenc_manage_driver_settings", "enabled" }, { "nvenc_vdd_force_vsync", "enabled" } };
  config::apply_command_line_overrides(reloaded, cli);
  EXPECT_EQ(reloaded.at("nvenc_manage_driver_settings"), "disabled");
  EXPECT_EQ(reloaded.at("nvenc_vdd_force_vsync"), "enabled");
}

TEST(ConfigOverrides, ExplicitVsyncOptInSurvivesRepeatedDiskReloads) {
  const std::unordered_map<std::string, std::string> cli { { "nvenc_vdd_force_vsync", "enabled" } };
  auto startup = std::unordered_map<std::string, std::string> { { "nvenc_vdd_force_vsync", "disabled" } };
  config::apply_command_line_overrides(startup, cli);
  EXPECT_EQ(startup.at("nvenc_vdd_force_vsync"), "enabled");
  auto reloaded = std::map<std::string, std::string> {};
  config::apply_command_line_overrides(reloaded, cli);
  EXPECT_EQ(reloaded.at("nvenc_vdd_force_vsync"), "enabled");
  reloaded = { { "nvenc_vdd_force_vsync", "disabled" }, { "nvenc_manage_driver_settings", "disabled" } };
  config::apply_command_line_overrides(reloaded, cli);
  EXPECT_EQ(reloaded.at("nvenc_vdd_force_vsync"), "enabled");
  EXPECT_EQ(reloaded.at("nvenc_manage_driver_settings"), "disabled");
}
