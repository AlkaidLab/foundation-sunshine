/**
 * @file tests/unit/test_ds5_config.cpp
 * @brief Tests for the standalone ds5_config.json store and runtime snapshot.
 */

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "../tests_common.h"
#include "src/ds5_config.h"

namespace {
  namespace fs = std::filesystem;

  class Ds5ConfigTest : public testing::Test {
  protected:
    void SetUp() override {
      root_ = fs::temp_directory_path() /
              ("sunshine_ds5_config_test_" +
               std::to_string(reinterpret_cast<std::uintptr_t>(this)));
      std::error_code ignored;
      fs::remove_all(root_, ignored);
      fs::create_directories(root_);
      path_ = root_ / "ds5_config.json";
      previous_ = ds5_config::current();
    }

    void TearDown() override {
      ds5_config::configure(previous_);
      std::error_code ignored;
      fs::remove_all(root_, ignored);
    }

    void write_json(const nlohmann::json &value) {
      std::ofstream file(path_, std::ios::binary | std::ios::trunc);
      file << value.dump(2) << '\n';
    }

    std::string read_text(const fs::path &path) {
      std::ifstream file(path, std::ios::binary);
      return {std::istreambuf_iterator<char> {file}, std::istreambuf_iterator<char> {}};
    }

    fs::path root_;
    fs::path path_;
    ds5_config::settings_t previous_;
  };

  nlohmann::json valid_json() {
    return {
      {"ds5_enabled", true},
      {"ds5_audio_haptics", false},
      {"ds5_legacy_haptics_strength", 1.5},
      {"ds5_legacy_haptics_curve", 0.5},
      {"ds5_legacy_haptics_noise_gate", 0.006},
    };
  }
}  // namespace

TEST_F(Ds5ConfigTest, ResolvesBesideSelectedSunshineConfig) {
  EXPECT_EQ(
    ds5_config::path_for(root_ / "sunshine.conf"),
    root_ / "ds5_config.json"
  );
  EXPECT_TRUE(ds5_config::path_for({}).empty());
}

TEST_F(Ds5ConfigTest, MissingFileReturnsDisabledDefaults) {
  const auto result = ds5_config::load(path_);
  EXPECT_EQ(result.status, ds5_config::load_status_t::MISSING);
  EXPECT_FALSE(result.settings.enabled);
  EXPECT_TRUE(result.settings.audio_haptics);
  EXPECT_DOUBLE_EQ(result.settings.legacy_strength, 1.0);
  EXPECT_DOUBLE_EQ(result.settings.legacy_curve, 1.0);
  EXPECT_DOUBLE_EQ(result.settings.legacy_noise_gate, 0.020);
}

TEST_F(Ds5ConfigTest, RejectsMalformedSchemaAndInvalidNumbers) {
  write_json({{"ds5_enabled", true}});
  EXPECT_EQ(ds5_config::load(path_).status, ds5_config::load_status_t::INVALID);

  auto input = valid_json();
  input["unexpected"] = true;
  write_json(input);
  EXPECT_EQ(ds5_config::load(path_).status, ds5_config::load_status_t::INVALID);

  auto invalid = ds5_config::settings_t {};
  invalid.legacy_strength = std::numeric_limits<double>::infinity();
  EXPECT_FALSE(ds5_config::validate(invalid));
  invalid = {};
  invalid.legacy_noise_gate = 0.061;
  EXPECT_FALSE(ds5_config::validate(invalid));
}

TEST_F(Ds5ConfigTest, SavesBacksUpAndReloadsCompleteSettings) {
  const ds5_config::settings_t previous {true, true, 1.2, 0.8, 0.010};
  const ds5_config::settings_t replacement {false, false, 2.0, 0.5, 0.006};

  ASSERT_TRUE(ds5_config::save(path_, previous));
  const auto previous_contents = read_text(path_);
  ASSERT_TRUE(ds5_config::save(path_, replacement));
  EXPECT_EQ(read_text(ds5_config::backup_path_for(path_)), previous_contents);

  const auto loaded = ds5_config::load(path_);
  ASSERT_EQ(loaded.status, ds5_config::load_status_t::LOADED);
  EXPECT_EQ(loaded.settings.enabled, replacement.enabled);
  EXPECT_EQ(loaded.settings.audio_haptics, replacement.audio_haptics);
  EXPECT_DOUBLE_EQ(loaded.settings.legacy_strength, replacement.legacy_strength);
  EXPECT_DOUBLE_EQ(loaded.settings.legacy_curve, replacement.legacy_curve);
  EXPECT_DOUBLE_EQ(loaded.settings.legacy_noise_gate, replacement.legacy_noise_gate);
}

TEST_F(Ds5ConfigTest, PreparedSnapshotDoesNotPublishUntilCommit) {
  const ds5_config::settings_t active {false, true, 1.0, 1.0, 0.020};
  const ds5_config::settings_t replacement {true, false, 2.0, 0.5, 0.006};
  ASSERT_TRUE(ds5_config::configure(active));

  auto prepared = ds5_config::prepare(replacement);
  ASSERT_TRUE(prepared);
  EXPECT_FALSE(ds5_config::current().enabled);
  ASSERT_TRUE(ds5_config::commit(std::move(prepared)));
  EXPECT_TRUE(ds5_config::current().enabled);
  EXPECT_DOUBLE_EQ(ds5_config::current().legacy_curve, 0.5);
}
