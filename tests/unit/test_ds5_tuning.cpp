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

TEST(Ds5TuningRuntime, RumbleTimingCombinesSendIntervalWithTheRateCap) {
  auto &state = ds5::tuning::runtime::state();
  state.rumble_send_interval_ms.store(20u);
  state.rumble_max_rate_hz.store(10u);
  EXPECT_EQ(ds5::tuning::runtime::rumble_tuning().effective_interval_ms, 100u);
  state.rumble_send_interval_ms.store(50u);
  state.rumble_max_rate_hz.store(1000u);
  EXPECT_EQ(ds5::tuning::runtime::rumble_tuning().effective_interval_ms, 50u);
  state.rumble_hold_ms.store(500u);
  state.rumble_watchdog_ms.store(1u);
  state.rumble_max_duration_ms.store(20000u);
  const auto clamped = ds5::tuning::runtime::rumble_tuning();
  EXPECT_EQ(clamped.hold_ms, 200u);
  EXPECT_EQ(clamped.watchdog_ms, 20u);
  EXPECT_EQ(clamped.max_duration_ms, 10000u);
}

TEST(Ds5TuningRuntime, SessionReleaseFadesTheLastLightbarAndCanHoldIt) {
  auto &state = ds5::tuning::runtime::state();
  state.lightbar_mode.store(1);
  state.lightbar_r.store(200);
  state.lightbar_g.store(80);
  state.lightbar_b.store(40);
  state.lightbar_brightness.store(1.0f);
  state.release_on_end.store(true);

  std::uint8_t red = 0;
  std::uint8_t green = 0;
  std::uint8_t blue = 0;
  ds5::tuning::runtime::apply_lightbar(red, green, blue, 1, 2, 3);
  EXPECT_EQ(red, 200u);
  EXPECT_EQ(green, 80u);
  EXPECT_EQ(blue, 40u);

  int steps = 0;
  std::uint8_t previous = 255;
  ds5::tuning::runtime::emit_session_release([&](std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    EXPECT_LT(r, previous);
    previous = r;
    red = r;
    green = g;
    blue = b;
    ++steps;
  });
  EXPECT_GT(steps, 1);
  EXPECT_EQ(red, 0u);
  EXPECT_EQ(green, 0u);
  EXPECT_EQ(blue, 0u);
  EXPECT_EQ(state.lightbar_out_r.load(), 0u);

  state.lightbar_out_r.store(200);
  state.lightbar_out_g.store(80);
  state.lightbar_out_b.store(40);
  state.release_on_end.store(false);
  EXPECT_FALSE(ds5::tuning::runtime::session_release_step(red, green, blue));
  EXPECT_EQ(state.lightbar_out_r.load(), 200u);
  EXPECT_EQ(state.lightbar_out_g.load(), 80u);
  EXPECT_EQ(state.lightbar_out_b.load(), 40u);
  state.release_on_end.store(true);
  state.lightbar_out_r.store(0);
  state.lightbar_out_g.store(0);
  state.lightbar_out_b.store(0);
}

TEST(Ds5TuningRuntime, CompatPresetChangesPcmAndTriggerBytes) {
  auto &state = ds5::tuning::runtime::state();
  state.compat_preset.store(0);
  EXPECT_TRUE(ds5::tuning::runtime::allow_authored_pcm());

  std::uint8_t left_mode = 0x21;
  std::uint8_t right_mode = 0x26;
  std::uint8_t left_params[10];
  std::uint8_t right_params[10];
  for (int index = 0; index < 10; ++index) {
    left_params[index] = static_cast<std::uint8_t>(index + 1);
    right_params[index] = static_cast<std::uint8_t>(20 + index);
  }
  ds5::tuning::runtime::apply_compat_triggers(left_mode, left_params, right_mode, right_params);
  EXPECT_EQ(left_mode, 0x21u);
  EXPECT_EQ(right_mode, 0x26u);
  EXPECT_EQ(left_params[0], 1u);
  EXPECT_EQ(right_params[9], 29u);

  state.compat_preset.store(2);
  EXPECT_TRUE(ds5::tuning::runtime::allow_authored_pcm());
  ds5::tuning::runtime::apply_compat_triggers(left_mode, left_params, right_mode, right_params);
  EXPECT_EQ(left_mode, 0x05u);
  EXPECT_EQ(right_mode, 0x05u);
  for (int index = 0; index < 10; ++index) {
    EXPECT_EQ(left_params[index], 0u) << index;
    EXPECT_EQ(right_params[index], 0u) << index;
  }

  state.compat_preset.store(1);
  EXPECT_FALSE(ds5::tuning::runtime::allow_authored_pcm());
  state.compat_preset.store(3);
  EXPECT_FALSE(ds5::tuning::runtime::allow_authored_pcm());
  state.compat_preset.store(0);
  EXPECT_TRUE(ds5::tuning::runtime::allow_authored_pcm());
}

TEST_F(Ds5TuningStore, PublishesReleaseAndCompatIntoTheRuntime) {
  ds5::tuning::store_t store(profile_path);
  ASSERT_TRUE(store.load());
  const auto result = store.apply_json({
    { "schema", "ALK-DS5T/1" },
    { "values", {
                  { "ds5.led.releaseOnEnd", false },
                  { "ds5.compat.preset", "legacyAll" },
                } },
  });
  ASSERT_TRUE(result.ok) << result.error;
  auto &state = ds5::tuning::runtime::state();
  EXPECT_FALSE(state.release_on_end.load());
  EXPECT_EQ(state.compat_preset.load(), 3u);
  EXPECT_FALSE(ds5::tuning::runtime::allow_authored_pcm());
  EXPECT_TRUE(key_entry(result.body, 89).at("implemented").get<bool>());
  EXPECT_TRUE(key_entry(result.body, 99).at("implemented").get<bool>());
  EXPECT_FALSE(key_entry(result.body, 89).at("value").get<bool>());
  EXPECT_EQ(key_entry(result.body, 99).at("value").get<std::string>(), "legacyAll");
}

TEST(Ds5TuningRuntime, TriggerThresholdDecidesTheDigitalBitAndAsButtonBinarisesTheAxis) {
  auto &state = ds5::tuning::runtime::state();
  state.left_trigger_threshold.store(0.5f);
  state.right_trigger_threshold.store(0.25f);
  state.left_trigger_as_button.store(false);
  state.right_trigger_as_button.store(true);

  EXPECT_FALSE(ds5::tuning::runtime::trigger_pressed(127, true));
  EXPECT_TRUE(ds5::tuning::runtime::trigger_pressed(128, true));
  EXPECT_FALSE(ds5::tuning::runtime::trigger_pressed(63, false));
  EXPECT_TRUE(ds5::tuning::runtime::trigger_pressed(64, false));

  std::uint8_t left = 200;
  ds5::tuning::runtime::apply_trigger_as_button(left, true, true);
  EXPECT_EQ(left, 200u);  // asButton off keeps the analog value

  std::uint8_t right = 70;
  ds5::tuning::runtime::apply_trigger_as_button(right, ds5::tuning::runtime::trigger_pressed(70, false), false);
  EXPECT_EQ(right, 255u);
  right = 10;
  ds5::tuning::runtime::apply_trigger_as_button(right, ds5::tuning::runtime::trigger_pressed(10, false), false);
  EXPECT_EQ(right, 0u);
  state.right_trigger_as_button.store(false);
  state.right_trigger_threshold.store(0.5f);
}

TEST_F(Ds5TuningStore, PublishesTriggerThresholdAndAsButtonIntoTheRuntime) {
  ds5::tuning::store_t store(profile_path);
  ASSERT_TRUE(store.load());
  const auto result = store.apply_json({
    { "schema", "ALK-DS5T/1" },
    { "values", {
                  { "gamepad.trigger.left.threshold", 0.8 },
                  { "gamepad.trigger.left.asButton", true },
                  { "gamepad.trigger.right.threshold", 0.1 },
                } },
  });
  ASSERT_TRUE(result.ok) << result.error;
  auto &state = ds5::tuning::runtime::state();
  EXPECT_NEAR(state.left_trigger_threshold.load(), 0.8f, 1e-6);
  EXPECT_TRUE(state.left_trigger_as_button.load());
  EXPECT_NEAR(state.right_trigger_threshold.load(), 0.1f, 1e-6);
  EXPECT_FALSE(state.right_trigger_as_button.load());
  for (const auto id : { 51u, 52u, 54u }) EXPECT_TRUE(key_entry(result.body, id).at("implemented").get<bool>()) << id;
}

TEST_F(Ds5TuningStore, TriggerThresholdAndAsButtonChangeWhatTheReportBuilderReceives) {
  ds5::tuning::store_t store(profile_path);
  ASSERT_TRUE(store.load());
  namespace rt = ds5::tuning::runtime;

  // Defaults: threshold 0.5, analog axes untouched.
  auto shaped = rt::shape_triggers(100, 200);
  EXPECT_FALSE(shaped.l2_pressed);
  EXPECT_TRUE(shaped.r2_pressed);
  EXPECT_EQ(shaped.left, 100u);
  EXPECT_EQ(shaped.right, 200u);

  // Keys 51 and 54 move the digital decision for each side independently; the axes stay analog.
  auto result = store.apply_json({ { "schema", "ALK-DS5T/1" },
    { "values", { { "gamepad.trigger.left.threshold", 0.25 }, { "gamepad.trigger.right.threshold", 0.9 } } } });
  ASSERT_TRUE(result.ok) << result.error;
  shaped = rt::shape_triggers(100, 200);
  EXPECT_TRUE(shaped.l2_pressed);
  EXPECT_FALSE(shaped.r2_pressed);
  EXPECT_EQ(shaped.left, 100u);
  EXPECT_EQ(shaped.right, 200u);

  // Keys 52 and 55 binarise the axis by that same decision, one side at a time.
  result = store.apply_json({ { "schema", "ALK-DS5T/1" }, { "values", { { "gamepad.trigger.left.asButton", true } } } });
  ASSERT_TRUE(result.ok) << result.error;
  shaped = rt::shape_triggers(100, 200);
  EXPECT_EQ(shaped.left, 255u);
  EXPECT_EQ(shaped.right, 200u);
  shaped = rt::shape_triggers(50, 200);
  EXPECT_EQ(shaped.left, 0u);
  EXPECT_FALSE(shaped.l2_pressed);

  result = store.apply_json({ { "schema", "ALK-DS5T/1" }, { "values", { { "gamepad.trigger.right.asButton", true } } } });
  ASSERT_TRUE(result.ok) << result.error;
  shaped = rt::shape_triggers(100, 200);
  EXPECT_EQ(shaped.right, 0u);  // 200/255 is below the 0.9 threshold
  EXPECT_FALSE(shaped.r2_pressed);
  shaped = rt::shape_triggers(100, 240);
  EXPECT_EQ(shaped.right, 255u);
  EXPECT_TRUE(shaped.r2_pressed);

  // The digital bit judges the raw byte, so a deadzone that zeroes the axis does not release it.
  result = store.apply_json({ { "schema", "ALK-DS5T/1" },
    { "values", { { "gamepad.trigger.left.asButton", false }, { "gamepad.trigger.left.deadzone", 0.6 } } } });
  ASSERT_TRUE(result.ok) << result.error;
  shaped = rt::shape_triggers(100, 0);
  EXPECT_EQ(shaped.left, 0u);
  EXPECT_TRUE(shaped.l2_pressed);
}

TEST_F(Ds5TuningStore, MigratesTheLegacyDualSenseSettingsOnceWithoutOverwriting) {
  {
    std::ofstream legacy(directory / "ds5_config.json");
    legacy << R"({"ds5_audio_haptics":false,"ds5_legacy_haptics_strength":2.5,"ds5_legacy_haptics_curve":0.7,"ds5_legacy_haptics_noise_gate":0.031,"ds5_genshin_compatibility":true})";
  }
  {
    ds5::tuning::store_t store(profile_path);
    ASSERT_TRUE(store.load());
    EXPECT_TRUE(std::filesystem::exists(profile_path));
    const auto snapshot = store.snapshot_json();
    EXPECT_EQ(key_entry(snapshot, 2).at("value").get<std::string>(), "off");
    EXPECT_NEAR(key_entry(snapshot, 14).at("value").get<double>(), 2.5, 1e-6);
    EXPECT_NEAR(key_entry(snapshot, 15).at("value").get<double>(), 0.7, 1e-6);
    EXPECT_NEAR(key_entry(snapshot, 18).at("value").get<double>(), 0.031, 1e-6);
    EXPECT_NEAR(ds5::tuning::runtime::state().rumble_strength.load(), 2.5f, 1e-6);
  }
  ASSERT_TRUE(std::filesystem::exists(directory / "ds5_config.json"));
  // The page changes a value; a later start must keep it and not read the legacy file again.
  {
    ds5::tuning::store_t store(profile_path);
    ASSERT_TRUE(store.load());
    ASSERT_TRUE(store.apply_json({ { "schema", "ALK-DS5T/1" }, { "values", { { "ds5.rumble.strength", 3.0 } } } }).ok);
  }
  ds5::tuning::store_t again(profile_path);
  ASSERT_TRUE(again.load());
  EXPECT_NEAR(key_entry(again.snapshot_json(), 14).at("value").get<double>(), 3.0, 1e-6);
  EXPECT_EQ(key_entry(again.snapshot_json(), 2).at("value").get<std::string>(), "off");
}

TEST_F(Ds5TuningStore, MigratesFromSunshineConfWhenNoLegacyFileExistsAndClampsOutOfRange) {
  {
    std::ofstream conf(directory / "sunshine.conf");
    conf << "port = 47991\nds5_legacy_haptics_strength = 9\nds5_audio_haptics = enabled\n";
  }
  ds5::tuning::store_t store(profile_path);
  ASSERT_TRUE(store.load());
  const auto snapshot = store.snapshot_json();
  EXPECT_NEAR(key_entry(snapshot, 14).at("value").get<double>(), 4.0, 1e-6);  // clamped to the descriptor range
  EXPECT_EQ(key_entry(snapshot, 2).at("value").get<std::string>(), "auto");
}

TEST_F(Ds5TuningStore, StartsWithDefaultsAndWritesNothingWhenThereIsNothingToMigrate) {
  ds5::tuning::store_t store(profile_path);
  ASSERT_TRUE(store.load());
  EXPECT_FALSE(std::filesystem::exists(profile_path));
}

TEST_F(Ds5TuningStore, PresetExpandsIntoHostKeysAndIsDerivedFromTheirValues) {
  ds5::tuning::store_t store(profile_path);
  ASSERT_TRUE(store.load());
  const auto result = store.apply_json({
    { "schema", "ALK-DS5T/1" },
    { "values", { { "ds5.profile.preset", "competitive" } } },
  });
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.code, 0u);

  auto &state = ds5::tuning::runtime::state();
  EXPECT_FALSE(state.rumble_enabled.load());
  EXPECT_EQ(state.lightbar_mode.load(), 1u);
  EXPECT_EQ(state.lightbar_r.load(), 0u);
  EXPECT_EQ(state.lightbar_g.load(), 120u);
  EXPECT_EQ(state.lightbar_b.load(), 255u);
  EXPECT_FLOAT_EQ(state.lightbar_brightness.load(), 0.35f);
  EXPECT_FLOAT_EQ(state.hd_gain.load(), 0.60f);

  auto snapshot = store.snapshot_json();
  EXPECT_EQ(key_entry(snapshot, 1).at("value").get<std::string>(), "competitive");
  EXPECT_FALSE(key_entry(snapshot, 1).at("stored").get<bool>());
  EXPECT_EQ(key_entry(snapshot, 81).at("value").get<std::string>(), "fixed");

  const auto custom = store.apply_json({
    { "schema", "ALK-DS5T/1" },
    { "values", { { "ds5.haptics.hd.gain", 1.0 } } },
  });
  ASSERT_TRUE(custom.ok) << custom.error;
  snapshot = store.snapshot_json();
  EXPECT_EQ(key_entry(snapshot, 1).at("value").get<std::string>(), "custom");
}

TEST_F(Ds5TuningStore, LegacyPresetWritesTheReleaseTriggerAndErmValues) {
  ds5::tuning::store_t store(profile_path);
  ASSERT_TRUE(store.load());
  const auto result = store.apply_json({
    { "schema", "ALK-DS5T/1" },
    { "values", { { "ds5.profile.preset", "legacy" } } },
  });
  ASSERT_TRUE(result.ok) << result.error;

  auto &state = ds5::tuning::runtime::state();
  EXPECT_EQ(state.compat_preset.load(), 3u);
  EXPECT_FALSE(ds5::tuning::runtime::allow_authored_pcm());
  EXPECT_FLOAT_EQ(state.hd_gain.load(), 0.0f);
  EXPECT_FLOAT_EQ(state.rumble_curve.load(), 0.5f);
  EXPECT_FLOAT_EQ(state.rumble_noise_gate.load(), 0.006f);

  std::uint8_t left_mode = 0x21;
  std::uint8_t right_mode = 0x26;
  std::uint8_t left_params[10] {};
  std::uint8_t right_params[10] {};
  ds5::tuning::runtime::apply_compat_triggers(left_mode, left_params, right_mode, right_params);
  EXPECT_EQ(left_mode, 0x05u);
  EXPECT_EQ(right_mode, 0x05u);

  const auto snapshot = store.snapshot_json();
  EXPECT_EQ(key_entry(snapshot, 99).at("value").get<std::string>(), "legacyAll");
  EXPECT_EQ(key_entry(snapshot, 1).at("value").get<std::string>(), "legacy");
}

TEST_F(Ds5TuningStore, DefaultPresetRestoresTheBundleAndCustomIsRejected) {
  ds5::tuning::store_t store(profile_path);
  ASSERT_TRUE(store.load());
  ASSERT_TRUE(store.apply_json({
                                 { "schema", "ALK-DS5T/1" },
                                 { "values", { { "ds5.profile.preset", "competitive" } } },
                               })
      .ok);
  const auto restored = store.apply_json({
    { "schema", "ALK-DS5T/1" },
    { "values", { { "ds5.profile.preset", "default" } } },
  });
  ASSERT_TRUE(restored.ok) << restored.error;
  auto &state = ds5::tuning::runtime::state();
  EXPECT_TRUE(state.rumble_enabled.load());
  EXPECT_EQ(state.lightbar_mode.load(), 0u);
  EXPECT_FLOAT_EQ(state.hd_gain.load(), 1.0f);
  EXPECT_EQ(key_entry(store.snapshot_json(), 1).at("value").get<std::string>(), "default");

  const auto custom = store.apply_json({
    { "schema", "ALK-DS5T/1" },
    { "values", { { "ds5.profile.preset", "custom" } } },
  });
  EXPECT_FALSE(custom.ok);
  EXPECT_EQ(custom.http_status, 400);
  EXPECT_EQ(custom.code, 3u);
}

TEST_F(Ds5TuningStore, IdentityProfileSelectsTheNextVirtualDeviceMode) {
  ds5::tuning::store_t store(profile_path);
  ASSERT_TRUE(store.load());
  const auto apply = [&](const char *name, std::uint32_t expected) {
    const auto result = store.apply_json({
      { "schema", "ALK-DS5T/1" },
      { "values", { { "ds5.identity.profile", name } } },
    });
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(ds5::tuning::runtime::identity_gamepad_mode(), expected) << name;
  };
  apply("xbox360", 2u);
  apply("ds4", 3u);
  apply("ds5", 4u);
  apply("auto", 0u);
}

TEST_F(Ds5TuningStore, TouchpadKeysPublishThePointerConfiguration) {
  ds5::tuning::store_t store(profile_path);
  ASSERT_TRUE(store.load());
  const auto result = store.apply_json({
    { "schema", "ALK-DS5T/1" },
    { "values", {
                  { "ds5.touchpad.output", "hostPointer" },
                  { "ds5.touchpad.sensitivity", 2.0 },
                  { "ds5.touchpad.scrollSpeed", 1.5 },
                  { "ds5.touchpad.reverseScroll", true },
                  { "ds5.touchpad.clickAction", "right" },
                  { "ds5.touchpad.twoFingerAction", "none" },
                } },
  });
  ASSERT_TRUE(result.ok) << result.error;
  const auto control = ds5::tuning::runtime::touchpad_control();
  EXPECT_EQ(control.output, ds5::tuning::runtime::TouchpadOutput::host_pointer);
  EXPECT_FLOAT_EQ(control.sensitivity, 2.0f);
  EXPECT_FLOAT_EQ(control.scroll_speed, 1.5f);
  EXPECT_TRUE(control.reverse_scroll);
  EXPECT_EQ(control.click, ds5::tuning::runtime::TouchpadClickAction::right);
  EXPECT_EQ(control.two_finger, ds5::tuning::runtime::TouchpadTwoFingerAction::none);
}

TEST_F(Ds5TuningStore, LegacyTouchpadModeMapsWhenOutputIsNotExplicit) {
  ds5::tuning::store_t store(profile_path);
  ASSERT_TRUE(store.load());
  ASSERT_TRUE(store.apply_json({
                                 { "schema", "ALK-DS5T/1" },
                                 { "values", { { "gamepad.touchpad.mode", "mouse" } } },
                               })
      .ok);
  EXPECT_EQ(ds5::tuning::runtime::touchpad_control().output, ds5::tuning::runtime::TouchpadOutput::host_pointer);

  ASSERT_TRUE(store.apply_json({
                                 { "schema", "ALK-DS5T/1" },
                                 { "values", { { "gamepad.touchpad.mode", "off" } } },
                               })
      .ok);
  EXPECT_EQ(ds5::tuning::runtime::touchpad_control().output, ds5::tuning::runtime::TouchpadOutput::off);

  ASSERT_TRUE(store.apply_json({
                                 { "schema", "ALK-DS5T/1" },
                                 { "values", { { "ds5.touchpad.output", "precision" } } },
                               })
      .ok);
  EXPECT_EQ(ds5::tuning::runtime::touchpad_control().output, ds5::tuning::runtime::TouchpadOutput::precision);
}

TEST(Ds5TuningTouchpad, SingleContactMovesByTheConfiguredSensitivity) {
  ds5::tuning::runtime::touchpad_pointer_t pointer;
  ds5::tuning::runtime::touchpad_control_t control;
  control.output = ds5::tuning::runtime::TouchpadOutput::host_pointer;
  control.sensitivity = 2.0f;
  control.surface_width = 1000;
  control.surface_height = 500;

  const auto down = pointer.touch(ds5::tuning::runtime::TouchPhase::down, 1, 0.5f, 0.5f, control);
  EXPECT_TRUE(down.consume);
  EXPECT_EQ(down.move_x, 0);
  EXPECT_EQ(down.move_y, 0);

  const auto move = pointer.touch(ds5::tuning::runtime::TouchPhase::move, 1, 0.6f, 0.4f, control);
  EXPECT_TRUE(move.consume);
  EXPECT_EQ(move.move_x, 200);
  EXPECT_EQ(move.move_y, -100);

  const auto up = pointer.touch(ds5::tuning::runtime::TouchPhase::up, 1, 0.6f, 0.4f, control);
  EXPECT_TRUE(up.consume);
  EXPECT_EQ(up.move_x, 0);
}

TEST(Ds5TuningTouchpad, TwoFingerScrollUsesTheMidpointAndCanReverse) {
  ds5::tuning::runtime::touchpad_pointer_t pointer;
  ds5::tuning::runtime::touchpad_control_t control;
  control.output = ds5::tuning::runtime::TouchpadOutput::host_pointer;
  control.surface_width = 1000;
  control.surface_height = 500;

  pointer.touch(ds5::tuning::runtime::TouchPhase::down, 1, 0.5f, 0.5f, control);
  pointer.touch(ds5::tuning::runtime::TouchPhase::down, 2, 0.5f, 0.5f, control);
  auto scroll = pointer.touch(ds5::tuning::runtime::TouchPhase::move, 1, 0.5f, 0.4f, control);
  EXPECT_EQ(scroll.move_x, 0);
  EXPECT_EQ(scroll.move_y, 0);
  EXPECT_EQ(scroll.scroll, 48);
  scroll = pointer.touch(ds5::tuning::runtime::TouchPhase::move, 2, 0.5f, 0.3f, control);
  EXPECT_EQ(scroll.scroll, 96);

  pointer.touch(ds5::tuning::runtime::TouchPhase::up, 1, 0.5f, 0.3f, control);
  pointer.touch(ds5::tuning::runtime::TouchPhase::up, 2, 0.5f, 0.3f, control);
  pointer.reset();
  control.reverse_scroll = true;
  pointer.touch(ds5::tuning::runtime::TouchPhase::down, 1, 0.5f, 0.5f, control);
  pointer.touch(ds5::tuning::runtime::TouchPhase::down, 2, 0.5f, 0.5f, control);
  scroll = pointer.touch(ds5::tuning::runtime::TouchPhase::move, 1, 0.5f, 0.4f, control);
  EXPECT_EQ(scroll.scroll, -48);
}

TEST(Ds5TuningTouchpad, TwoFingerRightClickHoldsUntilTheSecondContactLifts) {
  ds5::tuning::runtime::touchpad_pointer_t pointer;
  ds5::tuning::runtime::touchpad_control_t control;
  control.two_finger = ds5::tuning::runtime::TouchpadTwoFingerAction::right_click;

  pointer.touch(ds5::tuning::runtime::TouchPhase::down, 1, 0.5f, 0.5f, control);
  const auto press = pointer.touch(ds5::tuning::runtime::TouchPhase::down, 2, 0.6f, 0.5f, control);
  EXPECT_EQ(press.mouse_button, 3);
  EXPECT_FALSE(press.mouse_release);
  EXPECT_TRUE(press.consume);

  const auto release = pointer.touch(ds5::tuning::runtime::TouchPhase::up, 2, 0.6f, 0.5f, control);
  EXPECT_EQ(release.mouse_button, 3);
  EXPECT_TRUE(release.mouse_release);
}

TEST(Ds5TuningTouchpad, TouchpadButtonMapsToMouseOrStaysWithTheController) {
  ds5::tuning::runtime::touchpad_pointer_t pointer;
  ds5::tuning::runtime::touchpad_control_t control;

  control.click = ds5::tuning::runtime::TouchpadClickAction::left;
  auto press = pointer.button(true, control);
  EXPECT_EQ(press.mouse_button, 1);
  EXPECT_FALSE(press.mouse_release);
  EXPECT_TRUE(press.consume);
  auto release = pointer.button(false, control);
  EXPECT_EQ(release.mouse_button, 1);
  EXPECT_TRUE(release.mouse_release);

  control.click = ds5::tuning::runtime::TouchpadClickAction::touchpad_click;
  press = pointer.button(true, control);
  EXPECT_EQ(press.mouse_button, 0);
  EXPECT_FALSE(press.consume);
  pointer.button(false, control);

  control.click = ds5::tuning::runtime::TouchpadClickAction::none;
  press = pointer.button(true, control);
  EXPECT_EQ(press.mouse_button, 0);
  EXPECT_TRUE(press.consume);
}

TEST(Ds5TuningRuntime, PlayerMuteAndAudioOutputMappingsMatchTheReportFields) {
  auto &state = ds5::tuning::runtime::state();
  EXPECT_EQ(ds5::tuning::runtime::player_led_pattern(1), 0x04u);
  EXPECT_EQ(ds5::tuning::runtime::player_led_pattern(2), 0x0Au);
  EXPECT_EQ(ds5::tuning::runtime::player_led_pattern(3), 0x15u);
  EXPECT_EQ(ds5::tuning::runtime::player_led_pattern(4), 0x1Bu);
  EXPECT_EQ(ds5::tuning::runtime::player_led_pattern(5), 0x1Fu);

  state.player_led_mode.store(0);
  EXPECT_EQ(ds5::tuning::runtime::player_led_byte(0x1Fu), 0x1Fu);
  state.player_led_mode.store(1);
  state.player_led_index.store(3);
  EXPECT_EQ(ds5::tuning::runtime::player_led_byte(0x1Fu), 0x15u);
  state.player_led_mode.store(2);
  EXPECT_EQ(ds5::tuning::runtime::player_led_byte(0x1Fu), 0u);

  state.mute_led_mode.store(0);
  EXPECT_EQ(ds5::tuning::runtime::mute_led_byte(true), 1u);
  state.mute_led_mode.store(1);
  EXPECT_EQ(ds5::tuning::runtime::mute_led_byte(false), 1u);
  state.mute_led_mode.store(2);
  EXPECT_EQ(ds5::tuning::runtime::mute_led_byte(true), 0u);

  EXPECT_EQ(ds5::tuning::runtime::speaker_level_byte(0.0f), 0x3Du);
  EXPECT_EQ(ds5::tuning::runtime::speaker_level_byte(1.0f), 0x64u);
  EXPECT_EQ(ds5::tuning::runtime::headset_level_byte(0.0f), 0x00u);
  EXPECT_EQ(ds5::tuning::runtime::headset_level_byte(1.0f), 0x7Fu);
  EXPECT_EQ(ds5::tuning::runtime::mic_level_byte(0.0f), 0x00u);
  EXPECT_EQ(ds5::tuning::runtime::mic_level_byte(1.0f), 0x40u);
  EXPECT_EQ(ds5::tuning::runtime::audio_preamp_gain(0), 0u);
  EXPECT_EQ(ds5::tuning::runtime::audio_preamp_gain(1), 2u);
  EXPECT_EQ(ds5::tuning::runtime::audio_output_path(0, true), 0u);
  EXPECT_EQ(ds5::tuning::runtime::audio_output_path(0, false), 3u);
  EXPECT_EQ(ds5::tuning::runtime::audio_output_path(1, true), 3u);
  EXPECT_EQ(ds5::tuning::runtime::audio_output_path(2, false), 0u);
}

TEST(Ds5TuningDescriptors, WireExtensionKeysReportExactReasons) {
  const auto snapshot = ds5::tuning::store_t("/tmp/ds5-tuning-unused.json").snapshot_json();
  EXPECT_NE(key_entry(snapshot, 96).at("reason").get<std::string>().find("capture source"), std::string::npos);
  EXPECT_NE(key_entry(snapshot, 97).at("reason").get<std::string>().find("audio_control2"), std::string::npos);
  EXPECT_NE(key_entry(snapshot, 98).at("reason").get<std::string>().find("client"), std::string::npos);
  EXPECT_NE(key_entry(snapshot, 101).at("reason").get<std::string>().find("sampling"), std::string::npos);
  EXPECT_NE(key_entry(snapshot, 102).at("reason").get<std::string>().find("ALK-DS5T/1"), std::string::npos);
}

TEST(Ds5TuningRuntime, ExtensionTuningFoldsPlayerMuteVolumeAndRouteIntoTheOutputFields) {
  namespace ext = ds5::control_extension;
  auto &state = ds5::tuning::runtime::state();
  const auto reset = [&state] {
    state.player_led_mode.store(0);
    state.player_led_index.store(0);
    state.mute_led_mode.store(0);
    state.speaker_level.store(1.0f);
    state.headset_level.store(1.0f);
    state.mic_level.store(0.8f);
    state.speaker_mute.store(false);
    state.mic_mute.store(false);
    state.audio_route.store(0);
  };
  reset();
  // Defaults leave the game in control: nothing is added and nothing is changed.
  ext::output_t game;
  game.valid_mask = ext::out_speaker_volume | ext::out_player_leds;
  game.speaker_volume = 0x50;
  game.player_led_mask = 0x15;
  auto fields = game;
  ds5::tuning::runtime::apply_extension_tuning(fields, false);
  EXPECT_EQ(fields, game);

  state.player_led_mode.store(1);
  state.player_led_index.store(2);
  state.mute_led_mode.store(1);
  state.speaker_level.store(0.5f);
  state.headset_level.store(0.25f);
  state.mic_level.store(1.0f);
  state.audio_route.store(1);
  fields = {};
  ds5::tuning::runtime::apply_extension_tuning(fields, false);
  EXPECT_NE(fields.valid_mask & ext::out_player_leds, 0u);
  EXPECT_EQ(fields.player_led_mask, 0x0Au);  // fixed pattern for player 2
  EXPECT_NE(fields.valid_mask & ext::out_mute_led, 0u);
  EXPECT_EQ(fields.mute_led, 1u);
  EXPECT_EQ(fields.speaker_volume, ds5::tuning::runtime::speaker_level_byte(0.5f));
  EXPECT_EQ(fields.headphone_volume, ds5::tuning::runtime::headset_level_byte(0.25f));
  EXPECT_EQ(fields.mic_volume, 0x40u);
  EXPECT_EQ(fields.audio_routing, 3u);  // speaker path
  state.audio_route.store(2);
  fields = {};
  ds5::tuning::runtime::apply_extension_tuning(fields, false);
  EXPECT_EQ(fields.audio_routing, 0u);  // headset path

  state.player_led_mode.store(2);
  state.speaker_mute.store(true);
  state.mic_mute.store(true);
  fields = game;
  ds5::tuning::runtime::apply_extension_tuning(fields, true);
  EXPECT_EQ(fields.player_led_mask, 0u);
  EXPECT_EQ(fields.speaker_volume, 0u);  // mute wins over the game volume
  EXPECT_NE(fields.valid_mask & ext::out_mic_mute, 0u);
  EXPECT_EQ(fields.mic_mute, 1u);
  reset();
}

TEST_F(Ds5TuningStore, PublishesTheLedAndAudioKeysIntoTheRuntime) {
  ds5::tuning::store_t store(profile_path);
  ASSERT_TRUE(store.load());
  const auto result = store.apply_json({
    { "schema", "ALK-DS5T/1" },
    { "values", {
                  { "ds5.led.player.mode", "fixed" },
                  { "ds5.led.player.index", 3 },
                  { "ds5.led.mute.mode", "off" },
                  { "ds5.audio.speakerLevel", 0.4 },
                  { "ds5.audio.micMute", true },
                  { "ds5.audio.route", "headset" },
                } },
  });
  ASSERT_TRUE(result.ok) << result.error;
  auto &state = ds5::tuning::runtime::state();
  EXPECT_EQ(state.player_led_mode.load(), 1u);
  EXPECT_EQ(state.player_led_index.load(), 3u);
  EXPECT_EQ(state.mute_led_mode.load(), 2u);
  EXPECT_NEAR(state.speaker_level.load(), 0.4f, 1e-6);
  EXPECT_TRUE(state.mic_mute.load());
  EXPECT_EQ(state.audio_route.load(), 2u);
  for (const auto id : { 86u, 87u, 88u, 90u, 94u, 95u }) EXPECT_TRUE(key_entry(result.body, id).at("implemented").get<bool>()) << id;
  for (const auto id : { 96u, 97u }) EXPECT_FALSE(key_entry(result.body, id).at("implemented").get<bool>()) << id;
}

namespace {
  void
  reset_lightbar_state() {
    auto &state = ds5::tuning::runtime::state();
    state.lightbar_mode.store(0);
    state.lightbar_r.store(0);
    state.lightbar_g.store(0);
    state.lightbar_b.store(0);
    state.lightbar_brightness.store(1.0f);
  }

  int published_count = 0;
  void
  count_published() {
    ++published_count;
  }
}  // namespace

TEST(Ds5LightbarPush, FixedColourAndOffReachThePadWithoutAGameReport) {
  using ds5::tuning::runtime::plan_lightbar_push;
  reset_lightbar_state();
  auto &state = ds5::tuning::runtime::state();
  std::array<std::uint8_t, 3> game {};
  std::array<std::uint8_t, 3> shown {};
  // Follow game and no colour from the game yet: nothing to show.
  EXPECT_FALSE(plan_lightbar_push(false, game, shown, false).emit);
  // Fixed colour saved: shown now, although the game sent nothing.
  state.lightbar_mode.store(1);
  state.lightbar_r.store(52);
  state.lightbar_g.store(199);
  state.lightbar_b.store(89);
  auto push = plan_lightbar_push(false, game, shown, false);
  ASSERT_TRUE(push.emit);
  EXPECT_EQ(push.red, 52);
  EXPECT_EQ(push.green, 199);
  EXPECT_EQ(push.blue, 89);
  // Once it is shown, a second save of the same state sends nothing.
  shown = { push.red, push.green, push.blue };
  EXPECT_FALSE(plan_lightbar_push(false, game, shown, true).emit);
  // Off reaches the pad too, even when it never got a colour.
  state.lightbar_mode.store(2);
  push = plan_lightbar_push(false, game, std::array<std::uint8_t, 3> {}, false);
  EXPECT_TRUE(push.emit);
  EXPECT_EQ(push.red | push.green | push.blue, 0);
  reset_lightbar_state();
}

TEST(Ds5LightbarPush, FollowGameGoesBackToTheGamesColour) {
  using ds5::tuning::runtime::plan_lightbar_push;
  reset_lightbar_state();
  auto &state = ds5::tuning::runtime::state();
  const std::array<std::uint8_t, 3> game { 10, 20, 30 };
  // The pad shows a fixed colour; the saved mode is follow again and the game's last colour returns.
  const std::array<std::uint8_t, 3> shown { 52, 199, 89 };
  auto push = plan_lightbar_push(true, game, shown, true);
  ASSERT_TRUE(push.emit);
  EXPECT_EQ(push.red, 10);
  EXPECT_EQ(push.green, 20);
  EXPECT_EQ(push.blue, 30);
  // The saved brightness still scales what the game asked for.
  state.lightbar_brightness.store(0.5f);
  push = plan_lightbar_push(true, game, game, true);
  ASSERT_TRUE(push.emit);
  EXPECT_LT(push.red, 10);
  reset_lightbar_state();
}

TEST_F(Ds5TuningStore, APublishedProfileNotifiesTheHostSoPadsCanFollow) {
  published_count = 0;
  ds5::tuning::runtime::published_handler().store(&count_published);
  ds5::tuning::store_t store(profile_path);
  ASSERT_TRUE(store.load());
  const auto before = published_count;
  const auto result = store.apply_json({
    { "schema", "ALK-DS5T/1" },
    { "values", { { "ds5.led.lightbar.mode", "fixed" }, { "ds5.led.lightbar.colorR", 200 } } },
  });
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_GT(published_count, before);
  ds5::tuning::runtime::published_handler().store(nullptr);
  reset_lightbar_state();
}

TEST(Ds5GyroTuning, TheDeadzoneIsInDegreesPerSecondNotInCounts) {
  auto &state = ds5::tuning::runtime::state();
  state.gyro_activation.store(1);
  state.gyro_sensitivity.store(1.0f);
  state.gyro_orientation.store(0);
  state.gyro_yaw_offset_deg.store(0.0f);
  state.gyro_pitch_offset_deg.store(0.0f);
  state.gyro_roll_offset_deg.store(0.0f);
  state.gyro_filter.store(0);  // none
  state.gyro_deadzone.store(0.05f);
  ds5::tuning::runtime::motion_filter_state_t filter;
  // 0.05 of the 2048-count range at 16.4 counts per degree per second is 6.2 degrees per second. The earlier code
  // compared degrees per second with 102, which cut every rotation below 102 degrees per second.
  float x = 30.0f, y = 0.0f, z = 0.0f;
  ASSERT_TRUE(ds5::tuning::runtime::apply_gyro(x, y, z, filter));
  EXPECT_NEAR(x, 30.0f - 0.05f * 2048.0f / 16.4f, 0.01f);
  x = 3.0f;
  ASSERT_TRUE(ds5::tuning::runtime::apply_gyro(x, y, z, filter));
  EXPECT_EQ(x, 0.0f);
  // The default is small enough to leave slow aiming alone.
  state.gyro_deadzone.store(0.01f);
  x = 3.0f;
  ASSERT_TRUE(ds5::tuning::runtime::apply_gyro(x, y, z, filter));
  EXPECT_NEAR(x, 3.0f - 0.01f * 2048.0f / 16.4f, 0.01f);
  EXPECT_GT(x, 1.0f);
  state.gyro_deadzone.store(0.05f);
  state.gyro_filter.store(1);
}

TEST(Ds5GyroTuning, TheLowPassFollowsTheTimeBetweenSamples) {
  auto &state = ds5::tuning::runtime::state();
  state.gyro_activation.store(1);
  state.gyro_sensitivity.store(1.0f);
  state.gyro_orientation.store(0);
  state.gyro_yaw_offset_deg.store(0.0f);
  state.gyro_pitch_offset_deg.store(0.0f);
  state.gyro_roll_offset_deg.store(0.0f);
  state.gyro_deadzone.store(0.0f);
  state.gyro_filter.store(1);  // low pass
  state.gyro_filter_hz.store(30.0f);
  const auto first_step = [](float dt) {
    ds5::tuning::runtime::motion_filter_state_t filter;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    ds5::tuning::runtime::apply_gyro(x, y, z, filter, dt);  // starts the filter at zero
    x = 100.0f;
    y = z = 0.0f;
    ds5::tuning::runtime::apply_gyro(x, y, z, filter, dt);
    return x;
  };
  const auto alpha = [](float dt) { return 1.0f - std::exp(-2.0f * 3.14159265358979323846f * 30.0f * dt); };
  EXPECT_NEAR(first_step(1.0f / 250.0f), 100.0f * alpha(1.0f / 250.0f), 0.01f);
  EXPECT_NEAR(first_step(1.0f / 128.0f), 100.0f * alpha(1.0f / 128.0f), 0.01f);
  // At 128 samples per second the same cutoff takes a larger step than at 250: the old fixed factor made
  // a Bluetooth pad filter at about 15 Hz instead of 30.
  EXPECT_GT(first_step(1.0f / 128.0f), first_step(1.0f / 250.0f) + 20.0f);
  state.gyro_deadzone.store(0.05f);
}
