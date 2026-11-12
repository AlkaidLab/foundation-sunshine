/**
 * @file tests/unit/test_ds5_tuning.cpp
 * @brief ALK-DS5T/1 descriptor, validation and persistence tests.
 */
#include "src/ds5/tuning_profile.h"
#include "src/ds5/tuning_runtime.h"
#include "src/ds5/tuning_trigger.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <set>
#include <stdexcept>
#include <string>

namespace {
  using ds5::tuning::find_descriptor;

  class Ds5TuningStore: public ::testing::Test {
  protected:
    std::filesystem::path directory;
    std::filesystem::path profile_path;

    void
    SetUp() override {
      directory = std::filesystem::temp_directory_path() /
                  ("ds5-tuning-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
      std::filesystem::create_directories(directory);
      profile_path = directory / "dualsense_tuning.json";
    }

    void
    TearDown() override {
      std::error_code ignored;
      std::filesystem::remove_all(directory, ignored);
      auto &state = ds5::tuning::runtime::state();
      state.active_controllers.store(0, std::memory_order_release);
      state.rumble_enabled.store(true);
      state.rumble_strength.store(1.0f);
      state.rumble_low_gain.store(1.0f);
      state.rumble_high_gain.store(1.0f);
      state.rumble_curve.store(1.0f);
      state.rumble_noise_gate.store(0.020f);
      state.hd_gain.store(1.0f);
      state.haptics_mode.store(0);
      state.hd_low_band_db.store(0.0f);
      state.hd_high_band_db.store(0.0f);
      state.hd_crossover_hz.store(160.0f);
      state.hd_noise_gate.store(0.002f);
      state.hd_latency_ms.store(10);
      state.hd_smoothing_ms.store(2);
      state.ir_intensity.store(0.8f);
      state.ir_sharpness.store(0.5f);
      state.rumble_hold_ms.store(60);
      state.rumble_send_interval_ms.store(20);
      state.rumble_watchdog_ms.store(100);
      state.rumble_max_duration_ms.store(5000);
      state.rumble_max_rate_hz.store(250);
      state.trigger_enabled.store(true);
      state.trigger_strength.store(1.0f);
      state.trigger_left_strength.store(1.0f);
      state.trigger_right_strength.store(1.0f);
      state.trigger_feedback_scale.store(1.0f);
      state.trigger_weapon_scale.store(1.0f);
      state.trigger_vibration_scale.store(1.0f);
      state.trigger_slope_scale.store(1.0f);
      state.trigger_off_mode.store(0);
      state.trigger_raw_passthrough.store(false);
      state.trigger_mode_mask.store(31u);
      state.lightbar_mode.store(0);
      state.lightbar_r.store(0);
      state.lightbar_g.store(0);
      state.lightbar_b.store(0);
      state.lightbar_brightness.store(1.0f);
      state.lightbar_out_r.store(0);
      state.lightbar_out_g.store(0);
      state.lightbar_out_b.store(0);
      state.release_on_end.store(true);
      state.compat_preset.store(0);
      state.left_trigger_threshold.store(0.5f);
      state.right_trigger_threshold.store(0.5f);
      state.left_trigger_as_button.store(false);
      state.right_trigger_as_button.store(false);
      state.stick_shape.store(0);
      state.left_stick_deadzone_inner.store(0.0f);
      state.left_stick_deadzone_outer.store(1.0f);
      state.left_stick_anti_deadzone.store(0.0f);
      state.left_stick_sensitivity.store(1.0f);
      state.left_stick_curve.store(0);
      state.left_stick_invert_x.store(false);
      state.left_stick_invert_y.store(false);
      state.right_stick_deadzone_inner.store(0.0f);
      state.right_stick_deadzone_outer.store(1.0f);
      state.right_stick_anti_deadzone.store(0.0f);
      state.right_stick_sensitivity.store(1.0f);
      state.right_stick_curve.store(0);
      state.right_stick_invert_x.store(false);
      state.right_stick_invert_y.store(false);
      state.left_trigger_deadzone.store(0.0f);
      state.right_trigger_deadzone.store(0.0f);
      state.left_trigger_range_min.store(0.0f);
      state.left_trigger_range_max.store(1.0f);
      state.left_trigger_anti_deadzone.store(0.0f);
      state.left_trigger_curve.store(0);
      state.right_trigger_range_min.store(0.0f);
      state.right_trigger_range_max.store(1.0f);
      state.right_trigger_anti_deadzone.store(0.0f);
      state.right_trigger_curve.store(0);
      state.gyro_activation.store(1);
      state.gyro_sensitivity.store(1.0f);
      state.gyro_deadzone.store(0.05f);
      state.gyro_filter.store(1);
      state.gyro_filter_hz.store(30.0f);
      state.gyro_filter_beta.store(0.05f);
      state.gyro_orientation.store(0);
      state.gyro_yaw_offset_deg.store(0.0f);
      state.gyro_pitch_offset_deg.store(0.0f);
      state.gyro_roll_offset_deg.store(0.0f);
      state.touchpad_output.store(0);
      state.touchpad_sensitivity.store(1.0f);
      state.touchpad_scroll_speed.store(1.0f);
      state.touchpad_reverse_scroll.store(false);
      state.touchpad_click_action.store(0);
      state.touchpad_two_finger.store(0);
      state.identity_mode.store(0);
      state.player_led_mode.store(0);
      state.player_led_index.store(0);
      state.mute_led_mode.store(0);
      state.speaker_level.store(1.0f);
      state.headset_level.store(1.0f);
      state.mic_level.store(0.8f);
      state.speaker_mute.store(false);
      state.mic_mute.store(false);
      state.audio_route.store(0);
      state.mic_route.store(0);
      state.audio_preamp.store(0);
    }

    nlohmann::json
    snapshot(const ds5::tuning::store_t &store) {
      return store.snapshot_json();
    }
  };

  const nlohmann::json &
  key_entry(const nlohmann::json &snapshot, std::uint32_t id) {
    for (const auto &entry : snapshot.at("keys")) {
      if (entry.at("id").get<std::uint32_t>() == id) return entry;
    }
    throw std::runtime_error("key not found: " + std::to_string(id));
  }
}  // namespace

TEST(Ds5TuningDescriptors, CoversTheStableOneHundredSixKeys) {
  const auto &table = ds5::tuning::descriptors();
  ASSERT_EQ(table.size(), 106u);
  std::set<std::uint32_t> ids;
  std::set<std::string> keys;
  std::set<std::uint32_t> common_groups;
  std::uint32_t common_count = 0;
  for (const auto &descriptor : table) {
    EXPECT_TRUE(ids.insert(descriptor.id).second) << descriptor.id;
    EXPECT_TRUE(keys.insert(descriptor.key).second) << descriptor.key;
    ASSERT_GE(descriptor.id, 1u);
    ASSERT_LE(descriptor.id, 106u);
    if (descriptor.tier == ds5::tuning::Tier::common) {
      ++common_count;
      common_groups.insert(static_cast<std::uint32_t>(descriptor.group));
    }
    if (descriptor.type == ds5::tuning::ValueType::enumeration) {
      EXPECT_GT(descriptor.enum_count, 0u) << descriptor.key;
      EXPECT_NE(descriptor.enum_names, nullptr) << descriptor.key;
    }
    if (descriptor.type == ds5::tuning::ValueType::f32 ||
        descriptor.type == ds5::tuning::ValueType::u32 ||
        descriptor.type == ds5::tuning::ValueType::i32) {
      EXPECT_LE(descriptor.min_value, descriptor.max_value) << descriptor.key;
      EXPECT_GE(descriptor.default_number, descriptor.min_value) << descriptor.key;
      EXPECT_LE(descriptor.default_number, descriptor.max_value) << descriptor.key;
      EXPECT_GT(descriptor.step, 0.0) << descriptor.key;
    }
  }
  EXPECT_EQ(ids.size(), 106u);
  EXPECT_EQ(ids, ([] {
    std::set<std::uint32_t> value;
    for (std::uint32_t id = 1; id <= 106; ++id) value.insert(id);
    return value;
  }()));
  EXPECT_EQ(common_count, 14u);
  EXPECT_EQ(common_groups.size(), 8u);
}

TEST(Ds5TuningDescriptors, ReportsEveryKeyWithAHostConsumerAsImplemented) {
  const auto snapshot = ds5::tuning::store_t("/tmp/ds5-tuning-unused.json").snapshot_json();
  // Keys with no consumer on this host: Bluetooth output and fallback (10, 98, 100), the vendor slope
  // block (31), the microphone route and preamp (96, 97: no capture source and no wire field), the client
  // polling cap (101), cross-end sync (102 to 104) and the feedback target (105).
  const std::set<std::uint32_t> unavailable { 10, 31, 96, 97, 98, 100, 101, 102, 103, 104, 105 };
  EXPECT_EQ(snapshot.at("implemented_count").get<std::uint32_t>(), 106u - unavailable.size());
  for (std::uint32_t id = 1; id <= 106; ++id) {
    const bool implemented = key_entry(snapshot, id).at("implemented").get<bool>();
    EXPECT_EQ(implemented, unavailable.count(id) == 0) << id;
    if (!implemented) {
      EXPECT_EQ(key_entry(snapshot, id).at("status").get<std::string>(), "unavailable") << id;
      EXPECT_FALSE(key_entry(snapshot, id).at("reason").get<std::string>().empty()) << id;
    }
  }
}

TEST_F(Ds5TuningStore, PublishesInputTuningValuesToTheReportRuntime) {
  ds5::tuning::store_t store(profile_path);
  ASSERT_TRUE(store.load());
  const auto result = store.apply_json({
    { "schema", "ALK-DS5T/1" },
    { "values", {
                  { "gamepad.stick.left.deadzoneInner", 0.2 },
                  { "gamepad.stick.left.invertY", true },
                  { "ds5.axis.trigger.left.rangeMin", 0.1 },
                  { "gamepad.gyro.sensitivity", 1.5 },
                  { "ds5.gyro.filter", "oneEuro" },
                } },
  });
  ASSERT_TRUE(result.ok) << result.error;
  const auto &state = ds5::tuning::runtime::state();
  EXPECT_FLOAT_EQ(state.left_stick_deadzone_inner.load(), 0.2f);
  EXPECT_TRUE(state.left_stick_invert_y.load());
  EXPECT_FLOAT_EQ(state.left_trigger_range_min.load(), 0.1f);
  EXPECT_FLOAT_EQ(state.gyro_sensitivity.load(), 1.5f);
  EXPECT_EQ(state.gyro_filter.load(), 2u);
}

TEST(Ds5TuningDescriptors, ProfilePathLivesBesideTheConfigFile) {
  EXPECT_EQ(ds5::tuning::profile_path_for("/etc/sunshine/sunshine.conf", "/var/lib/sunshine"),
    std::filesystem::path("/etc/sunshine/dualsense_tuning.json"));
  EXPECT_EQ(ds5::tuning::profile_path_for("sunshine.conf", "/var/lib/sunshine"),
    std::filesystem::path("/var/lib/sunshine/dualsense_tuning.json"));
}

TEST_F(Ds5TuningStore, ClampsNumericValuesToTheDescriptorRange) {
  ds5::tuning::store_t store(profile_path);
  ASSERT_TRUE(store.load());
  const auto result = store.apply_json({
    { "schema", "ALK-DS5T/1" },
    { "values", { { "ds5.haptics.hd.gain", 9.0 }, { "ds5.rumble.strength", 0.01 } } },
  });
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.http_status, 200);
  EXPECT_EQ(result.code, 0u);
  EXPECT_TRUE(result.body.at("applied").at(0).at("clamped").get<bool>());
  EXPECT_DOUBLE_EQ(key_entry(result.body, 3).at("value").get<double>(), 2.0);
  EXPECT_DOUBLE_EQ(key_entry(result.body, 14).at("value").get<double>(), 0.1);
  EXPECT_TRUE(std::filesystem::exists(profile_path));
}

TEST_F(Ds5TuningStore, RejectsUnknownKeysWrongTypesAndBadEnumsWithoutWriting) {
  ds5::tuning::store_t store(profile_path);
  ASSERT_TRUE(store.load());

  const auto unknown = store.apply_json({
    { "schema", "ALK-DS5T/1" },
    { "values", { { "ds5.not.a.key", 1 } } },
  });
  EXPECT_FALSE(unknown.ok);
  EXPECT_EQ(unknown.http_status, 400);
  EXPECT_EQ(unknown.code, 9u);

  const auto wrong_type = store.apply_json({
    { "schema", "ALK-DS5T/1" },
    { "values", { { "ds5.haptics.hd.gain", "loud" } } },
  });
  EXPECT_FALSE(wrong_type.ok);
  EXPECT_EQ(wrong_type.http_status, 400);
  EXPECT_EQ(wrong_type.code, 3u);

  const auto bad_enum = store.apply_json({
    { "schema", "ALK-DS5T/1" },
    { "values", { { "ds5.led.lightbar.mode", "rainbow" } } },
  });
  EXPECT_FALSE(bad_enum.ok);
  EXPECT_EQ(bad_enum.http_status, 400);
  EXPECT_EQ(bad_enum.code, 3u);

  const auto no_schema = store.apply_json({
    { "values", { { "ds5.haptics.hd.gain", 1.0 } } },
  });
  EXPECT_FALSE(no_schema.ok);
  EXPECT_EQ(no_schema.http_status, 400);
  EXPECT_FALSE(std::filesystem::exists(profile_path));
}

TEST_F(Ds5TuningStore, PersistsAcrossStoreInstances) {
  std::uint64_t revision = 0;
  {
    ds5::tuning::store_t store(profile_path);
    ASSERT_TRUE(store.load());
    const auto result = store.apply_json({
      { "schema", "ALK-DS5T/1" },
      { "values", {
                    { "ds5.haptics.hd.gain", 1.4 },
                    { "ds5.rumble.enabled", false },
                    { "ds5.rumble.strength", 2.0 },
                    { "ds5.led.lightbar.mode", "fixed" },
                    { "ds5.led.lightbar.colorR", 255 },
                    { "ds5.led.lightbar.colorG", 128 },
                    { "ds5.led.lightbar.colorB", 0 },
                  } },
    });
    ASSERT_TRUE(result.ok) << result.error;
    revision = result.body.at("revision").get<std::uint64_t>();
    EXPECT_EQ(revision, 1u);
  }

  ds5::tuning::store_t reloaded(profile_path);
  std::string error;
  ASSERT_TRUE(reloaded.load(&error)) << error;
  const auto snapshot = reloaded.snapshot_json();
  EXPECT_EQ(snapshot.at("revision").get<std::uint64_t>(), revision);
  EXPECT_DOUBLE_EQ(key_entry(snapshot, 3).at("value").get<double>(), 1.4);
  EXPECT_FALSE(key_entry(snapshot, 13).at("value").get<bool>());
  EXPECT_DOUBLE_EQ(key_entry(snapshot, 14).at("value").get<double>(), 2.0);
  EXPECT_EQ(key_entry(snapshot, 81).at("value").get<std::string>(), "fixed");
  EXPECT_EQ(key_entry(snapshot, 82).at("value").get<std::uint32_t>(), 255u);
  EXPECT_EQ(key_entry(snapshot, 83).at("value").get<std::uint32_t>(), 128u);
  EXPECT_TRUE(key_entry(snapshot, 83).at("stored").get<bool>());
}

TEST_F(Ds5TuningStore, ReportsUnwiredKeysAsUnavailableAndDoesNotPersistThem) {
  ds5::tuning::store_t store(profile_path);
  ASSERT_TRUE(store.load());
  const auto result = store.apply_json({
    { "schema", "ALK-DS5T/1" },
    { "values", { { "ds5.trigger.adaptive.slopeScale", 1.5 } } },
  });
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.http_status, 409);
  EXPECT_EQ(result.code, 5u);
  ASSERT_EQ(result.body.at("unavailable").size(), 1u);
  EXPECT_EQ(result.body.at("unavailable").at(0).at("code").get<std::uint32_t>(), 5u);
  EXPECT_FALSE(result.body.at("unavailable").at(0).at("reason").get<std::string>().empty());
  EXPECT_FALSE(std::filesystem::exists(profile_path));
}

TEST_F(Ds5TuningStore, AppliesWiredKeysAndReportsUnwiredKeysInOnePatch) {
  ds5::tuning::store_t store(profile_path);
  ASSERT_TRUE(store.load());
  const auto result = store.apply_json({
    { "schema", "ALK-DS5T/1" },
    { "values", {
                  { "ds5.haptics.hd.gain", 1.2 },
                  { "ds5.trigger.adaptive.slopeScale", 1.5 },
                } },
  });
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.http_status, 200);
  EXPECT_EQ(result.code, 1u);
  EXPECT_TRUE(result.body.at("partial").get<bool>());
  EXPECT_EQ(result.body.at("applied").size(), 1u);
  EXPECT_EQ(result.body.at("unavailable").size(), 1u);

  ds5::tuning::store_t reloaded(profile_path);
  ASSERT_TRUE(reloaded.load());
  const auto snapshot = reloaded.snapshot_json();
  EXPECT_DOUBLE_EQ(key_entry(snapshot, 3).at("value").get<double>(), 1.2);
  EXPECT_DOUBLE_EQ(key_entry(snapshot, 31).at("value").get<double>(), 1.0);
  EXPECT_FALSE(key_entry(snapshot, 31).at("stored").get<bool>());
}

TEST_F(Ds5TuningStore, MarksSavedImplementationsAsAppliedWhileTheNativePathIsActive) {
  auto &state = ds5::tuning::runtime::state();
  state.active_controllers.store(1, std::memory_order_release);
  ds5::tuning::store_t store(profile_path);
  ASSERT_TRUE(store.load());
  const auto snapshot = store.snapshot_json();
  EXPECT_EQ(key_entry(snapshot, 3).at("status").get<std::string>(), "applied");
  EXPECT_TRUE(key_entry(snapshot, 3).at("active").get<bool>());
  state.active_controllers.store(0, std::memory_order_release);
  const auto idle = store.snapshot_json();
  EXPECT_EQ(key_entry(idle, 3).at("status").get<std::string>(), "ready");
}

TEST(Ds5TuningRuntime, RumbleShapingAppliesGainCurveAndNoiseGate) {
  auto &state = ds5::tuning::runtime::state();
  state.rumble_enabled.store(true);
  state.rumble_strength.store(0.5f);
  state.rumble_low_gain.store(1.0f);
  state.rumble_high_gain.store(1.0f);
  state.rumble_curve.store(1.0f);
  state.rumble_noise_gate.store(0.02f);

  std::uint16_t left = 65535;
  std::uint16_t right = 1000;
  ds5::tuning::runtime::apply_rumble(left, right);
  EXPECT_EQ(left, 32768u);
  EXPECT_EQ(right, 0u);

  state.rumble_enabled.store(false);
  left = right = 50000;
  ds5::tuning::runtime::apply_rumble(left, right);
  EXPECT_EQ(left, 0u);
  EXPECT_EQ(right, 0u);
}

TEST(Ds5TuningRuntime, LightbarOverrideUsesFixedColourAndBrightness) {
  auto &state = ds5::tuning::runtime::state();
  state.lightbar_mode.store(1);
  state.lightbar_r.store(200);
  state.lightbar_g.store(100);
  state.lightbar_b.store(50);
  state.lightbar_brightness.store(0.5f);

  std::uint8_t red = 9;
  std::uint8_t green = 9;
  std::uint8_t blue = 9;
  ds5::tuning::runtime::apply_lightbar(red, green, blue, 10, 20, 30);
  EXPECT_EQ(red, 100u);
  EXPECT_EQ(green, 50u);
  EXPECT_EQ(blue, 25u);

  state.lightbar_mode.store(2);
  ds5::tuning::runtime::apply_lightbar(red, green, blue, 10, 20, 30);
  EXPECT_EQ(red, 0u);
  EXPECT_EQ(green, 0u);
  EXPECT_EQ(blue, 0u);
}

TEST(Ds5TuningRuntime, PcmGainScalesAndClampsSamples) {
  auto &state = ds5::tuning::runtime::state();
  state.hd_gain.store(0.5f);
  std::int16_t samples[] = { 1000, -1000, 32767 };
  ds5::tuning::runtime::apply_pcm_gain(reinterpret_cast<std::uint8_t *>(samples), sizeof(samples));
  EXPECT_EQ(samples[0], 500);
  EXPECT_EQ(samples[1], -500);
  EXPECT_EQ(samples[2], 16384);

  state.hd_gain.store(0.0f);
  ds5::tuning::runtime::apply_pcm_gain(reinterpret_cast<std::uint8_t *>(samples), sizeof(samples));
  EXPECT_EQ(samples[0], 0);
  EXPECT_EQ(samples[1], 0);
  EXPECT_EQ(samples[2], 0);
}

TEST(Ds5TuningRuntime, StickShapingAppliesDeadzoneCurveGainInversionAndSaturation) {
  auto &state = ds5::tuning::runtime::state();
  state.stick_shape.store(0);
  state.left_stick_deadzone_inner.store(0.2f);
  state.left_stick_deadzone_outer.store(0.8f);
  state.left_stick_anti_deadzone.store(0.1f);
  state.left_stick_sensitivity.store(2.0f);
  state.left_stick_curve.store(0);
  state.left_stick_invert_y.store(true);

  std::int16_t x = 16384;
  std::int16_t y = 16384;
  ds5::tuning::runtime::apply_stick(x, y, true);
  EXPECT_EQ(x, 32767);
  EXPECT_EQ(y, -32768);

  x = y = 0;
  ds5::tuning::runtime::apply_stick(x, y, true);
  EXPECT_EQ(x, 0);
  EXPECT_EQ(y, 0);
}

TEST(Ds5TuningRuntime, TriggerShapingAppliesRangeCurveAntiDeadzoneAndSaturation) {
  auto &state = ds5::tuning::runtime::state();
  state.left_trigger_deadzone.store(0.0f);
  state.left_trigger_range_min.store(0.2f);
  state.left_trigger_range_max.store(0.8f);
  state.left_trigger_anti_deadzone.store(0.1f);
  state.left_trigger_curve.store(2);

  std::uint8_t trigger = 128;
  ds5::tuning::runtime::apply_trigger(trigger, true);
  EXPECT_NEAR(trigger, 78, 1);

  trigger = 255;
  ds5::tuning::runtime::apply_trigger(trigger, true);
  EXPECT_EQ(trigger, 255);
}

TEST(Ds5TuningRuntime, GyroShapingAppliesActivationOrientationGainAndFilter) {
  auto &state = ds5::tuning::runtime::state();
  state.gyro_activation.store(1);
  state.gyro_sensitivity.store(2.0f);
  state.gyro_deadzone.store(0.0f);
  state.gyro_filter.store(0);
  state.gyro_orientation.store(3);
  state.gyro_yaw_offset_deg.store(90.0f);
  ds5::tuning::runtime::motion_filter_state_t filter;

  float x = 10.0f;
  float y = 0.0f;
  float z = 0.0f;
  ASSERT_TRUE(ds5::tuning::runtime::apply_gyro(x, y, z, filter));
  EXPECT_NEAR(x, 0.0f, 0.01f);
  EXPECT_NEAR(y, 20.0f, 0.01f);
  EXPECT_NEAR(z, 0.0f, 0.01f);

  state.gyro_activation.store(0);
  x = 10.0f;
  y = z = 1.0f;
  EXPECT_FALSE(ds5::tuning::runtime::apply_gyro(x, y, z, filter));
  EXPECT_FLOAT_EQ(x, 0.0f);
  EXPECT_FLOAT_EQ(y, 0.0f);
  EXPECT_FLOAT_EQ(z, 0.0f);
}

namespace {
  using ds5::tuning::trigger::Action;
  using ds5::tuning::trigger::block_t;
  using ds5::tuning::trigger::config_t;

  class Ds5TriggerTest: public ::testing::Test {
  protected:
    void
    SetUp() override {
      reset_trigger_state();
    }

    void
    TearDown() override {
      reset_trigger_state();
    }

    static void
    reset_trigger_state() {
      auto &state = ds5::tuning::runtime::state();
      state.trigger_enabled.store(true);
      state.trigger_strength.store(1.0f);
      state.trigger_left_strength.store(1.0f);
      state.trigger_right_strength.store(1.0f);
      state.trigger_feedback_scale.store(1.0f);
      state.trigger_weapon_scale.store(1.0f);
      state.trigger_vibration_scale.store(1.0f);
      state.trigger_slope_scale.store(1.0f);
      state.trigger_off_mode.store(0);
      state.trigger_raw_passthrough.store(false);
      state.trigger_mode_mask.store(31u);
    }

    static block_t
    block(std::initializer_list<std::uint8_t> values) {
      block_t result {};
      std::copy(values.begin(), values.end(), result.begin());
      return result;
    }
  };
}  // namespace

TEST_F(Ds5TriggerTest, NeutralStrengthPassesKnownAndRawBlocksByteForByte) {
  const auto config = ds5::tuning::trigger::read_config();
  const std::array<block_t, 5> inputs {
    block({ 0x01, 0x03, 0xC8, 0, 0, 0, 0, 0, 0, 0, 0 }),
    block({ 0x02, 0x01, 0x06, 0xC8, 0, 0, 0, 0, 0, 0, 0 }),
    block({ 0x21, 0x03, 0x00, 0x1F, 0, 0, 0, 0, 0, 0, 0 }),
    block({ 0x26, 0x03, 0x00, 0x1F, 0, 0, 0, 0, 0, 0x30, 0 }),
    block({ 0x77, 0xAA, 0x55, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08 }),
  };
  for (const auto &input : inputs) {
    block_t output {};
    Action action {};
    EXPECT_TRUE(ds5::tuning::trigger::rewrite(input, false, config, output, action));
    EXPECT_EQ(action, Action::passthrough);
    EXPECT_EQ(output, input);
  }
}

TEST_F(Ds5TriggerTest, FeedbackAndWeaponStrengthChangeOnlyTheirForceByte) {
  auto config = ds5::tuning::trigger::read_config();
  config.strength = 1.5f;
  const auto feedback = block({ 0x01, 0x03, 200, 0, 0, 0, 0, 0, 0, 0, 0 });
  block_t output {};
  Action action {};
  ASSERT_TRUE(ds5::tuning::trigger::rewrite(feedback, false, config, output, action));
  EXPECT_EQ(action, Action::replace);
  EXPECT_EQ(output[0], 0x01);
  EXPECT_EQ(output[1], 0x03);
  EXPECT_EQ(output[2], 255);
  EXPECT_EQ(output[3], 0);

  config.strength = 0.5f;
  const auto weapon = block({ 0x02, 0x01, 0x06, 200, 0, 0, 0, 0, 0, 0, 0 });
  ASSERT_TRUE(ds5::tuning::trigger::rewrite(weapon, false, config, output, action));
  EXPECT_EQ(action, Action::replace);
  EXPECT_EQ(output[1], 0x01);
  EXPECT_EQ(output[2], 0x06);
  EXPECT_EQ(output[3], 100);
}

TEST_F(Ds5TriggerTest, ZoneModesScaleEachThreeBitLevelAndKeepFrequency) {
  auto config = ds5::tuning::trigger::read_config();
  config.strength = 0.5f;
  const auto vibration = block({ 0x26, 0x03, 0x00, 0x1F, 0, 0, 0, 0, 0, 0x30, 0 });
  block_t output {};
  Action action {};
  ASSERT_TRUE(ds5::tuning::trigger::rewrite(vibration, false, config, output, action));
  EXPECT_EQ(action, Action::replace);
  EXPECT_EQ(output[0], 0x26);
  EXPECT_EQ(output[1], 0x03);
  EXPECT_EQ(output[2], 0x00);
  // Zone 0: level 8 -> 4 (stored 3). Zone 1: level 4 -> 2 (stored 1).
  EXPECT_EQ(output[3], 0x0B);
  EXPECT_EQ(output[4], 0x00);
  EXPECT_EQ(output[9], 0x30);
}

TEST_F(Ds5TriggerTest, ModernWeaponAndSimpleVibrationFollowTheirModeScale) {
  auto config = ds5::tuning::trigger::read_config();
  block_t output {};
  Action action {};

  config.weapon_scale = 0.5f;
  const auto weapon = block({ 0x25, 0x04, 0x01, 0x07, 0, 0, 0, 0, 0, 0, 0 });
  ASSERT_TRUE(ds5::tuning::trigger::rewrite(weapon, false, config, output, action));
  EXPECT_EQ(action, Action::replace);
  EXPECT_EQ(output[0], 0x25);
  EXPECT_EQ(output[1], 0x04);
  EXPECT_EQ(output[2], 0x01);
  // Level 8 -> 4, stored as 3.
  EXPECT_EQ(output[3], 0x03);

  config.weapon_scale = 2.0f;
  ASSERT_TRUE(ds5::tuning::trigger::rewrite(weapon, false, config, output, action));
  EXPECT_EQ(action, Action::passthrough);

  config.weapon_scale = 0.0f;
  ASSERT_TRUE(ds5::tuning::trigger::rewrite(weapon, false, config, output, action));
  EXPECT_EQ(output, ds5::tuning::trigger::off_block());

  config.weapon_scale = 1.0f;
  config.vibration_scale = 0.5f;
  const auto vibration = block({ 0x06, 0x20, 200, 0x03, 0, 0, 0, 0, 0, 0, 0 });
  ASSERT_TRUE(ds5::tuning::trigger::rewrite(vibration, false, config, output, action));
  EXPECT_EQ(action, Action::replace);
  EXPECT_EQ(output[1], 0x20);
  EXPECT_EQ(output[2], 100);
  EXPECT_EQ(output[3], 0x03);

  config.vibration_scale = 0.0f;
  ASSERT_TRUE(ds5::tuning::trigger::rewrite(vibration, false, config, output, action));
  EXPECT_EQ(output, ds5::tuning::trigger::off_block());
}

TEST_F(Ds5TriggerTest, DisableMaskAndOffModeSelectTheDocumentedClear) {
  auto config = ds5::tuning::trigger::read_config();
  const auto feedback = block({ 0x01, 0x03, 200, 0, 0, 0, 0, 0, 0, 0, 0 });
  block_t output {};
  Action action {};

  config.enabled = false;
  ASSERT_TRUE(ds5::tuning::trigger::rewrite(feedback, false, config, output, action));
  EXPECT_EQ(action, Action::replace);
  EXPECT_EQ(output[0], 0x05);
  EXPECT_EQ(output[1], 0);

  config.enabled = true;
  config.mode_mask = 30u;  // feedback bit cleared
  ASSERT_TRUE(ds5::tuning::trigger::rewrite(feedback, false, config, output, action));
  EXPECT_EQ(action, Action::replace);
  EXPECT_EQ(output[0], 0x05);

  config.mode_mask = 31u;
  config.enabled = false;
  config.off_mode = ds5::tuning::trigger::OffMode::hold;
  EXPECT_FALSE(ds5::tuning::trigger::rewrite(feedback, false, config, output, action));
  EXPECT_EQ(action, Action::none);

  config.off_mode = ds5::tuning::trigger::OffMode::clear;
  config.enabled = false;
  ASSERT_TRUE(ds5::tuning::trigger::rewrite(feedback, false, config, output, action));
  EXPECT_EQ(action, Action::replace_then_zero);
  EXPECT_EQ(output[0], 0x05);
}

TEST_F(Ds5TriggerTest, RawBlocksNeedPassthroughWhenStrengthIsNotNeutral) {
  auto config = ds5::tuning::trigger::read_config();
  config.strength = 1.25f;
  const auto raw = block({ 0x77, 0xAA, 0x55, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08 });
  block_t output {};
  Action action {};

  ASSERT_TRUE(ds5::tuning::trigger::rewrite(raw, false, config, output, action));
  EXPECT_EQ(action, Action::replace);
  EXPECT_EQ(output[0], 0x05);

  config.raw_passthrough = true;
  ASSERT_TRUE(ds5::tuning::trigger::rewrite(raw, false, config, output, action));
  EXPECT_EQ(action, Action::passthrough);
  EXPECT_EQ(output, raw);
}

TEST_F(Ds5TriggerTest, PerSideStrengthAppliesToTheMatchingBlock) {
  auto config = ds5::tuning::trigger::read_config();
  config.left_strength = 0.5f;
  config.right_strength = 2.0f;
  const auto feedback = block({ 0x01, 0x03, 200, 0, 0, 0, 0, 0, 0, 0, 0 });
  block_t left {};
  block_t right {};
  Action left_action {};
  Action right_action {};
  ASSERT_TRUE(ds5::tuning::trigger::rewrite(feedback, true, config, left, left_action));
  ASSERT_TRUE(ds5::tuning::trigger::rewrite(feedback, false, config, right, right_action));
  EXPECT_EQ(left[2], 100);
  EXPECT_EQ(right[2], 255);
}

TEST_F(Ds5TriggerTest, ModeScalesApplyIndependentlyOfTheGlobalStrength) {
  auto config = ds5::tuning::trigger::read_config();
  block_t output {};
  Action action {};

  config.feedback_scale = 0.5f;
  const auto feedback = block({ 0x01, 0x03, 200, 0, 0, 0, 0, 0, 0, 0, 0 });
  ASSERT_TRUE(ds5::tuning::trigger::rewrite(feedback, false, config, output, action));
  EXPECT_EQ(output[2], 100);

  config.feedback_scale = 1.0f;
  config.weapon_scale = 2.0f;
  const auto weapon = block({ 0x02, 0x02, 0x08, 100, 0, 0, 0, 0, 0, 0, 0 });
  ASSERT_TRUE(ds5::tuning::trigger::rewrite(weapon, false, config, output, action));
  EXPECT_EQ(output[3], 200);

  config.weapon_scale = 1.0f;
  config.vibration_scale = 0.5f;
  const auto vibration = block({ 0x26, 0x01, 0x00, 0x07, 0, 0, 0, 0, 0, 0, 0 });
  ASSERT_TRUE(ds5::tuning::trigger::rewrite(vibration, false, config, output, action));
  EXPECT_EQ(output[3], 0x03);
}

TEST_F(Ds5TriggerTest, CaptureDoesNotResendAnUnchangedBlock) {
  const auto config = ds5::tuning::trigger::read_config();
  block_t state = block({ 0x01, 0x03, 200, 0, 0, 0, 0, 0, 0, 0, 0 });
  block_t output {};
  Action action {};
  EXPECT_FALSE(ds5::tuning::trigger::capture(state, false, config, state, output, action));
  EXPECT_EQ(action, Action::none);
}

TEST_F(Ds5TriggerTest, TrackerAppliesPolicyAndStrengthChangesWithoutANewGameBlock) {
  auto config = ds5::tuning::trigger::read_config();
  ds5::tuning::trigger::tracker_t tracker;
  std::array<std::uint8_t, 48> report {};
  const auto right = block({ 0x01, 0x03, 200, 0, 0, 0, 0, 0, 0, 0, 0 });
  const auto left = block({ 0x02, 0x02, 0x08, 200, 0, 0, 0, 0, 0, 0, 0 });
  std::copy(right.begin(), right.end(), report.begin() + 11);
  std::copy(left.begin(), left.end(), report.begin() + 22);

  const auto first = tracker.plan(report.data(), 0x0Cu, config);
  EXPECT_EQ(first.flags, 0x0Cu);
  EXPECT_EQ(tracker.plan(report.data(), 0x0Cu, config).flags, 0u);

  config.enabled = false;
  const auto disabled = tracker.plan(report.data(), 0u, config);
  EXPECT_EQ(disabled.flags, 0x0Cu);
  EXPECT_EQ(disabled.left[0], 0x05);
  EXPECT_EQ(disabled.right[0], 0x05);

  config.enabled = true;
  config.strength = 0.5f;
  const auto scaled = tracker.plan(report.data(), 0u, config);
  EXPECT_EQ(scaled.flags, 0x0Cu);
  EXPECT_EQ(scaled.left[3], 100);
  EXPECT_EQ(scaled.right[2], 100);
}

TEST(Ds5TuningRuntime, HapticsTuningMapsDbClampsAndMode) {
  auto &state = ds5::tuning::runtime::state();
  state.hd_low_band_db.store(6.0206f);
  state.hd_high_band_db.store(-6.0206f);
  state.hd_crossover_hz.store(100.0f);
  state.hd_noise_gate.store(0.01f);
  state.ir_intensity.store(0.5f);
  state.ir_sharpness.store(1.0f);
  state.hd_latency_ms.store(50u);
  state.hd_smoothing_ms.store(0u);
  const auto tuning = ds5::tuning::runtime::haptics_tuning();
  EXPECT_NEAR(tuning.low_gain, 2.0f, 0.001f);
  EXPECT_NEAR(tuning.high_gain, 0.5f, 0.001f);
  EXPECT_FLOAT_EQ(tuning.crossover_hz, 100.0f);
  EXPECT_FLOAT_EQ(tuning.noise_gate, 0.01f);
  EXPECT_FLOAT_EQ(tuning.intensity, 0.5f);
  EXPECT_FLOAT_EQ(tuning.sharpness, 1.0f);
  EXPECT_EQ(tuning.latency_ms, 50u);
  EXPECT_EQ(tuning.smoothing_ms, 0u);
  EXPECT_TRUE(tuning.enabled);

  state.haptics_mode.store(3u);
  EXPECT_FALSE(ds5::tuning::runtime::haptics_tuning().enabled);
  EXPECT_FALSE(ds5::tuning::runtime::pcm_source_enabled(true));
  state.haptics_mode.store(0u);
  EXPECT_TRUE(ds5::tuning::runtime::pcm_source_enabled(true));
  EXPECT_FALSE(ds5::tuning::runtime::pcm_source_enabled(false));
}
