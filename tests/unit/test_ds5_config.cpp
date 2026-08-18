/**
 * @file tests/unit/test_ds5_config.cpp
 * @brief Tests for the standalone ds5_config.json store and runtime snapshot.
 */

#include <cstdint>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <thread>
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

  nlohmann::json extended_json() {
    auto value = valid_json();
    value["ds5_legacy_haptics_schema"] = 2;
    value["ds5_legacy_haptics_profile"] = "balanced";
    value["ds5_legacy_haptics_max_output"] = 0.70;
    value["ds5_legacy_haptics_high_scale"] = 0.75;
    value["ds5_legacy_haptics_response"] = "balanced";
    value["ds5_legacy_haptics_body_mix"] = 0.15;
    return value;
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
  EXPECT_EQ(result.settings.revision, 1);
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
  invalid = {};
  invalid.legacy_max_output = 0.24;
  EXPECT_FALSE(ds5_config::validate(invalid));
  invalid = {};
  invalid.legacy_body_mix = 0.36;
  EXPECT_FALSE(ds5_config::validate(invalid));

  write_json(extended_json());
  const auto extended = ds5_config::load(path_);
  ASSERT_EQ(extended.status, ds5_config::load_status_t::LOADED);
  EXPECT_EQ(extended.settings.legacy_profile, ds5_config::legacy_profile_t::balanced);
  EXPECT_DOUBLE_EQ(extended.settings.legacy_max_output, 0.70);
  EXPECT_DOUBLE_EQ(extended.settings.legacy_high_scale, 0.75);
  EXPECT_EQ(extended.settings.legacy_response, ds5_config::legacy_response_t::balanced);
  EXPECT_DOUBLE_EQ(extended.settings.legacy_body_mix, 0.15);
}

TEST_F(Ds5ConfigTest, ResolvesLegacyPresetsToCompleteRendererSettings) {
  auto settings = ds5_config::settings_t {};
  settings.legacy_profile = ds5_config::legacy_profile_t::balanced;
  const auto resolved = ds5_config::resolve_legacy_profile(settings);
  EXPECT_DOUBLE_EQ(resolved.legacy_strength, 1.0);
  EXPECT_DOUBLE_EQ(resolved.legacy_curve, 0.5);
  EXPECT_DOUBLE_EQ(resolved.legacy_noise_gate, 0.006);
  EXPECT_DOUBLE_EQ(resolved.legacy_max_output, 0.70);
  EXPECT_DOUBLE_EQ(resolved.legacy_high_scale, 0.75);
  EXPECT_EQ(resolved.legacy_response, ds5_config::legacy_response_t::balanced);
  EXPECT_DOUBLE_EQ(resolved.legacy_body_mix, 0.15);

  settings.legacy_profile = ds5_config::legacy_profile_t::custom;
  settings.legacy_strength = 1.7;
  EXPECT_DOUBLE_EQ(ds5_config::resolve_legacy_profile(settings).legacy_strength, 1.7);
}

TEST_F(Ds5ConfigTest, SavesBacksUpAndReloadsCompleteSettings) {
  const ds5_config::settings_t previous {true, true, 1.2, 0.8, 0.010};
  auto replacement = ds5_config::settings_t {false, false, 2.0, 0.5, 0.006};
  replacement.revision = 9;

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
  EXPECT_EQ(loaded.settings.legacy_profile, replacement.legacy_profile);
  EXPECT_DOUBLE_EQ(loaded.settings.legacy_max_output, replacement.legacy_max_output);
  EXPECT_DOUBLE_EQ(loaded.settings.legacy_high_scale, replacement.legacy_high_scale);
  EXPECT_EQ(loaded.settings.legacy_response, replacement.legacy_response);
  EXPECT_DOUBLE_EQ(loaded.settings.legacy_body_mix, replacement.legacy_body_mix);
  // Revision describes only the current process and is not persisted.
  EXPECT_EQ(loaded.settings.revision, 1);
}

TEST_F(Ds5ConfigTest, PreparedSnapshotDoesNotPublishUntilCommit) {
  const ds5_config::settings_t active {false, true, 1.0, 1.0, 0.020};
  auto replacement = ds5_config::settings_t {true, false, 2.0, 0.5, 0.006};
  replacement.revision = active.revision + 1;
  ASSERT_TRUE(ds5_config::configure(active));

  auto prepared = ds5_config::prepare(replacement);
  ASSERT_TRUE(prepared);
  EXPECT_FALSE(ds5_config::current().enabled);
  ASSERT_TRUE(ds5_config::commit(std::move(prepared)));
  EXPECT_TRUE(ds5_config::current().enabled);
  EXPECT_DOUBLE_EQ(ds5_config::current().legacy_curve, 0.5);
  EXPECT_EQ(ds5_config::current().revision, replacement.revision);
}

TEST_F(Ds5ConfigTest, ConcurrentReadersObserveOnlyCompleteSnapshots) {
  ds5_config::settings_t first {false, true, 1.0, 1.0, 0.020};
  ds5_config::settings_t second {true, false, 4.0, 0.3, 0.002};
  first.revision = 1;
  second.revision = 2;
  ASSERT_TRUE(ds5_config::configure(first));

  std::atomic_bool running {true};
  std::atomic_bool invalid_snapshot {false};
  std::thread reader([&]() {
    while (running.load(std::memory_order_relaxed)) {
      const auto observed = ds5_config::current();
      const bool is_first = !observed.enabled && observed.audio_haptics &&
                            observed.legacy_strength == 1.0 && observed.legacy_curve == 1.0 &&
                            observed.legacy_noise_gate == 0.020 && observed.revision == 1;
      const bool is_second = observed.enabled && !observed.audio_haptics &&
                             observed.legacy_strength == 4.0 && observed.legacy_curve == 0.3 &&
                             observed.legacy_noise_gate == 0.002 && observed.revision == 2;
      if (!is_first && !is_second) {
        invalid_snapshot.store(true, std::memory_order_relaxed);
        break;
      }
    }
  });

  for (int iteration = 0; iteration < 2000; ++iteration) {
    if (!ds5_config::configure(iteration % 2 == 0 ? second : first)) {
      invalid_snapshot.store(true, std::memory_order_relaxed);
      break;
    }
  }
  running.store(false, std::memory_order_relaxed);
  reader.join();
  EXPECT_FALSE(invalid_snapshot.load(std::memory_order_relaxed));
}
