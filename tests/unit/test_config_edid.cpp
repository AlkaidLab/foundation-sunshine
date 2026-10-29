/** @file test_config_edid.cpp
 * Preserve the EDID preference across legacy saves without preventing resets.
 */
#include "src/config.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <system_error>

#include <gtest/gtest.h>

namespace {
  class EdidConfigSave: public testing::Test {
  protected:
    void SetUp() override {
      original_path_ = config::sunshine.config_file;
      path_ = std::filesystem::temp_directory_path() /
        ("sunshine_edid_config_test_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".conf");
      config::sunshine.config_file = path_.string();
      std::ofstream(path_) << "vdd_edid_native_resolution = 3840x2160\nsunshine_name = old-name\n";
    }

    void TearDown() override {
      config::sunshine.config_file = original_path_;
      std::error_code ignored;
      std::filesystem::remove(path_, ignored);
    }

  private:
    std::string original_path_;
    std::filesystem::path path_;
  };
}

TEST_F(EdidConfigSave, LegacySavePreservesLatestPersistedPreference) {
  // A newer client may have saved a choice that has not been applied/reloaded.
  ASSERT_TRUE(config::update_full_config({{"vdd_edid_native_resolution", "2736x1824"}}));
  ASSERT_TRUE(config::update_full_config({{"sunshine_name", "legacy-client"}}));
  const auto saved = config::get_config_snapshot();
  ASSERT_TRUE(saved);
  EXPECT_EQ(saved->at("vdd_edid_native_resolution"), "2736x1824");
  EXPECT_EQ(saved->at("sunshine_name"), "legacy-client");
}

TEST_F(EdidConfigSave, ExplicitDefaultResetsPreferenceAndSurvivesLegacySave) {
  ASSERT_TRUE(config::update_full_config({{"vdd_edid_native_resolution", "default"}}));
  ASSERT_TRUE(config::update_full_config({{"sunshine_name", "legacy-client"}}));
  const auto saved = config::get_config_snapshot();
  ASSERT_TRUE(saved);
  EXPECT_EQ(saved->at("vdd_edid_native_resolution"), "default");
}

TEST_F(EdidConfigSave, LegacySaveDoesNotAddAnUnconfiguredPreference) {
  ASSERT_TRUE(config::update_full_config({{"vdd_edid_native_resolution", ""}}));
  ASSERT_TRUE(config::update_full_config({{"sunshine_name", "legacy-client"}}));
  const auto saved = config::get_config_snapshot();
  ASSERT_TRUE(saved);
  EXPECT_EQ(saved->count("vdd_edid_native_resolution"), 0u);
}
