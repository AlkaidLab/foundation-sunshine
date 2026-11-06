/** @file test_config_edid.cpp
 * Preserve the EDID preference across legacy saves without preventing resets.
 */
#include "src/config.h"
#include "src/config_test_support.h"
#include "src/display_device/vdd_monitor_command.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <system_error>

#include <gtest/gtest.h>

namespace {
  class EdidConfigSave: public testing::Test {
  protected:
    /** @brief Isolate the host file and startup override map so persistence/runtime checks cannot affect real settings. */
    void
    SetUp() override {
      original_overrides_ = config::test_support::exchange_command_line_overrides({});
      original_path_ = config::sunshine.config_file;
      path_ = std::filesystem::temp_directory_path() /
              ("sunshine_edid_config_test_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".conf");
      config::sunshine.config_file = path_.string();
      std::ofstream(path_) << "vdd_edid_native_resolution = 3840x2160\nsunshine_name = old-name\n"
                              "resolutions = [1920x1080,3840x2160]\nfps = [60,120]\n";
    }

    /** @brief Restore the original override policy and host path, then remove the isolated configuration. */
    void
    TearDown() override {
      config::test_support::exchange_command_line_overrides(std::move(original_overrides_));
      config::sunshine.config_file = original_path_;
      std::error_code ignored;
      std::filesystem::remove(path_, ignored);
    }

  private:
    std::unordered_map<std::string, std::string> original_overrides_;
    std::string original_path_;
    std::filesystem::path path_;
  };
}  // namespace

/** @brief A legacy save must retain a newer client's persisted custom choice. */
TEST_F(EdidConfigSave, LegacySavePreservesLatestPersistedPreference) {
  // A newer client may have saved a choice that has not been applied/reloaded.
  ASSERT_TRUE(config::update_full_config({ { "vdd_edid_native_resolution", "2736x1824" },
    { "resolutions", "[1920x1080,2736x1824,3840x2160]" } }));
  ASSERT_TRUE(config::update_full_config({ { "sunshine_name", "legacy-client" } }));
  const auto saved = config::get_config_snapshot();
  ASSERT_TRUE(saved);
  EXPECT_EQ(saved->at("vdd_edid_native_resolution"), "2736x1824");
  EXPECT_EQ(saved->at("sunshine_name"), "legacy-client");
}

/** @brief An explicit Default reset survives subsequent older-client saves. */
TEST_F(EdidConfigSave, ExplicitDefaultResetsPreferenceAndSurvivesLegacySave) {
  ASSERT_TRUE(config::update_full_config({ { "vdd_edid_native_resolution", "default" } }));
  ASSERT_TRUE(config::update_full_config({ { "sunshine_name", "legacy-client" } }));
  const auto saved = config::get_config_snapshot();
  ASSERT_TRUE(saved);
  EXPECT_EQ(saved->at("vdd_edid_native_resolution"), "default");
}

/** @brief An omitted preference is not synthesized after an explicit clear. */
TEST_F(EdidConfigSave, LegacySaveDoesNotAddAnUnconfiguredPreference) {
  ASSERT_TRUE(config::update_full_config({ { "vdd_edid_native_resolution", "" } }));
  ASSERT_TRUE(config::update_full_config({ { "sunshine_name", "legacy-client" } }));
  const auto saved = config::get_config_snapshot();
  ASSERT_TRUE(saved);
  EXPECT_EQ(saved->count("vdd_edid_native_resolution"), 0u);
}

/** @brief Reject removal of the retained native size without touching the saved configuration. */
TEST_F(EdidConfigSave, LegacyRemovalOfSelectedSizeRejectsWithoutWriting) {
  const auto before = config::get_config_snapshot();
  ASSERT_TRUE(before);
  EXPECT_FALSE(config::update_full_config({ { "resolutions", "[1920x1080]" }, { "fps", "[60,120]" } }));
  EXPECT_EQ(config::get_config_snapshot(), before);
}

/** @brief Reject a mode-list update that removes the last encodable native refresh. */
TEST_F(EdidConfigSave, LegacyRemovalOfEncodableRefreshRejectsWithoutWriting) {
  const auto before = config::get_config_snapshot();
  ASSERT_TRUE(before);
  EXPECT_FALSE(config::update_full_config({ { "fps", "[120,144]" } }));
  EXPECT_EQ(config::get_config_snapshot(), before);
}

/** @brief A compatible advertised-mode change preserves the selected native size. */
TEST_F(EdidConfigSave, LegacySupportedModeChangeRetainsSelection) {
  ASSERT_TRUE(config::update_full_config({ { "resolutions", "[2560x1440,3840x2160]" }, { "fps", "[60]" } }));
  const auto saved = config::get_config_snapshot();
  ASSERT_TRUE(saved);
  EXPECT_EQ(saved->at("vdd_edid_native_resolution"), "3840x2160");
}

/** @brief Resetting native selection permits intentional removal of its former size. */
TEST_F(EdidConfigSave, ExplicitDefaultAllowsRemovalOfSelectedSize) {
  ASSERT_TRUE(config::update_full_config({ { "resolutions", "[1920x1080]" }, { "fps", "[120]" },
    { "vdd_edid_native_resolution", "default" } }));
  const auto saved = config::get_config_snapshot();
  ASSERT_TRUE(saved);
  EXPECT_EQ(saved->at("vdd_edid_native_resolution"), "default");
  EXPECT_EQ(saved->at("resolutions"), "[1920x1080]");
}

/** @brief A legacy partial save retains the lists against which its EDID choice was validated. */
TEST_F(EdidConfigSave, OmittedListsAndPreferenceRetainTheirLatestSavedValues) {
  ASSERT_TRUE(config::update_full_config({ { "sunshine_name", "partial-client" } }));
  const auto saved = config::get_config_snapshot();
  ASSERT_TRUE(saved);
  EXPECT_EQ(saved->at("resolutions"), "[1920x1080,3840x2160]");
  EXPECT_EQ(saved->at("fps"), "[60,120]");
  EXPECT_EQ(saved->at("vdd_edid_native_resolution"), "3840x2160");
}

/** @brief Submitting one list never deletes the other list. */
TEST_F(EdidConfigSave, PartialRefreshChangePreservesResolutions) {
  ASSERT_TRUE(config::update_full_config({ { "fps", "[60]" } }));
  const auto saved = config::get_config_snapshot();
  ASSERT_TRUE(saved);
  EXPECT_EQ(saved->at("resolutions"), "[1920x1080,3840x2160]");
  EXPECT_EQ(saved->at("fps"), "[60]");
}

/** @brief Explicit empty lists remain empty instead of silently restoring old values. */
TEST_F(EdidConfigSave, ExplicitEmptyListsAreNotRestored) {
  ASSERT_TRUE(config::update_full_config({ { "vdd_edid_native_resolution", "default" },
    { "resolutions", "[]" }, { "fps", "[]" } }));
  const auto saved = config::get_config_snapshot();
  ASSERT_TRUE(saved);
  EXPECT_EQ(saved->at("resolutions"), "[]");
  EXPECT_EQ(saved->at("fps"), "[]");
}

/** @brief Recheck an explicit partial preference against the actual merged lists under the save lock. */
TEST_F(EdidConfigSave, UnsupportedExplicitPartialSelectionDoesNotWrite) {
  const auto before = config::get_config_snapshot();
  ASSERT_TRUE(before);
  EXPECT_FALSE(config::update_full_config({ { "vdd_edid_native_resolution", "2736x1824" } }));
  EXPECT_EQ(config::get_config_snapshot(), before);
}

/** @brief Runtime snapshots retain CLI precedence while observing later disk saves. */
TEST_F(EdidConfigSave, MonitorCommandUsesEffectivePreferenceAndLatestSavedValues) {
  config::test_support::exchange_command_line_overrides({ { "vdd_edid_native_resolution", "default" } });
  auto effective = config::get_effective_config_snapshot();
  ASSERT_TRUE(effective);
  EXPECT_EQ(effective->at("vdd_edid_native_resolution"), "default");
  EXPECT_EQ(config::get_config_snapshot()->at("vdd_edid_native_resolution"), "3840x2160");
  const auto command = [&](const auto &settings) {
    return display_device::vdd_edid::create_monitor_command("{00000001-0002-0003-0405-060708090a0b}",
      1000, 0.0001f, 1000, 0, 0, settings.at("vdd_edid_native_resolution"));
  };
  EXPECT_EQ(command(*effective).find("[edid="), std::string::npos);
  ASSERT_TRUE(config::update_full_config({ { "vdd_edid_native_resolution", "1920x1080" } }));
  effective = config::get_effective_config_snapshot();
  ASSERT_TRUE(effective);
  EXPECT_EQ(effective->at("vdd_edid_native_resolution"), "default");
  config::test_support::exchange_command_line_overrides({ { "vdd_edid_native_resolution", "3840x2160" } });
  effective = config::get_effective_config_snapshot();
  ASSERT_TRUE(effective);
  EXPECT_NE(command(*effective).find("[edid=3840x2160]"), std::string::npos);
  config::test_support::exchange_command_line_overrides({});
  effective = config::get_effective_config_snapshot();
  ASSERT_TRUE(effective);
  EXPECT_NE(command(*effective).find("[edid=1920x1080]"), std::string::npos);
}
