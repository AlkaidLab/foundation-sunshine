/**
 * @file src/ds5/tuning_profile.cpp
 * @brief ALK-DS5T/1 descriptor table, validation and persistence.
 *
 * Sources: docs/architecture/ds5-platform-design.zh.md sections 16.4.1-16.4.12
 * (branch docs/ds5-tuning-profile, commit 29f7673fb, read 2026-10-31) for the
 * key table and UI metadata; ds5-linux-hid-playstation-20261031 for the
 * output-report field and audio ranges that are applied by the native path;
 * ds5-rfc8259-json-20261031 for the profile document shape.
 */
#include "tuning_profile.h"
#include "tuning_runtime.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <fstream>
#include <sstream>
#include <system_error>
#include <type_traits>
#include <utility>

namespace ds5::tuning {
  using namespace std::literals;

  namespace {
    const char *const preset_names[] = { "default", "competitive", "immersive", "legacy", "custom" };
    const char *const haptics_mode_names[] = { "auto", "off" };
    const char *const bt_fallback_names[] = { "auto", "ir", "rumble", "off" };
    const char *const off_mode_names[] = { "release", "clear", "hold" };
    const char *const mode_mask_names[] = { "feedback", "weapon", "vibration", "slope", "raw" };
    const char *const stick_shape_names[] = { "perAxis", "radial" };
    const char *const curve_names[] = { "linear", "enhanced", "quadratic", "cubic", "easeOutQuad", "easeOutCubic" };
    const char *const gyro_activation_names[] = { "off", "hostRequested" };
    const char *const gyro_filter_names[] = { "none", "lowPass", "oneEuro" };
    const char *const gyro_orientation_names[] = { "device", "screen", "world", "custom" };
    const char *const touchpad_mode_names[] = { "off", "mouse", "passthrough", "precision" };
    const char *const touchpad_output_names[] = { "controller", "hostPointer", "precision" };
    const char *const click_action_names[] = { "touchpadClick", "left", "right", "none" };
    const char *const two_finger_names[] = { "scroll", "rightClick", "none" };
    const char *const led_mode_names[] = { "followGame", "fixed", "off" };
    const char *const player_index_names[] = { "auto", "1", "2", "3", "4", "5" };
    const char *const mute_mode_names[] = { "followGame", "on", "off" };
    const char *const audio_route_names[] = { "auto", "speaker", "headset" };
    const char *const mic_route_names[] = { "controller", "host", "none" };
    const char *const preamp_names[] = { "0dB", "+6dB" };
    const char *const bt_audio_fallback_names[] = { "speakerOnly", "headsetOnly", "off" };
    const char *const compat_preset_names[] = { "none", "legacyRumble", "legacyTrigger", "legacyAll" };
    const char *const compat_bt_output_names[] = { "auto", "legacy", "ir", "off" };
    const char *const feedback_target_names[] = { "auto", "controller1", "controller2", "controller3", "controller4" };
    const char *const identity_names[] = { "auto", "xbox360", "ds4", "ds5" };

    const descriptor_t k_descriptor_table[] = {
#include "tuning_descriptors.inc"
    };

    const std::vector<descriptor_t> &
    table() {
      static const std::vector<descriptor_t> value(std::begin(k_descriptor_table), std::end(k_descriptor_table));
      return value;
    }

    const std::map<std::uint32_t, const descriptor_t *> &
    by_id() {
      static const std::map<std::uint32_t, const descriptor_t *> value = [] {
        std::map<std::uint32_t, const descriptor_t *> result;
        for (const auto &descriptor : table()) result.emplace(descriptor.id, &descriptor);
        return result;
      }();
      return value;
    }

    const std::map<std::string_view, const descriptor_t *> &
    by_key() {
      static const std::map<std::string_view, const descriptor_t *> value = [] {
        std::map<std::string_view, const descriptor_t *> result;
        for (const auto &descriptor : table()) result.emplace(descriptor.key, &descriptor);
        return result;
      }();
      return value;
    }

    std::string
    value_type_name(ValueType type) {
      switch (type) {
        case ValueType::boolean:
          return "bool";
        case ValueType::u32:
          return "u32";
        case ValueType::i32:
          return "i32";
        case ValueType::f32:
          return "f32";
        case ValueType::enumeration:
          return "enum";
      }
      return "unknown";
    }

    nlohmann::json
    value_to_json(const value_t &value) {
      return std::visit([](const auto &entry) -> nlohmann::json {
        using entry_t = std::decay_t<decltype(entry)>;
        if constexpr (std::is_same_v<entry_t, float>) {
          if (!std::isfinite(entry)) return nullptr;
          // Keep the wire/UI value stable at the descriptor step precision.
          return std::round(static_cast<double>(entry) * 1000000.0) / 1000000.0;
        }
        else {
          return entry;
        }
      },
        value);
    }

    /**
     * @brief Values written by one named preset.
     *
     * The design defines the preset names but no per-key bundle table; this
     * table is the concrete product policy.  It only contains host keys that
     * the native path implements, so a preset never writes an unavailable
     * value.  Design section 16.4.2 gives the ERM noise gate and curve used by
     * the legacy bundle; section 16.8 maps the legacy compatibility preset to
     * legacyAll.
     */
    using preset_bundle_t = std::vector<std::pair<std::uint32_t, value_t>>;

    const preset_bundle_t &
    preset_bundle(std::string_view name) {
      static const preset_bundle_t default_bundle {
        { 3, 1.0f },
        { 13, true },
        { 14, 1.0f },
        { 15, 1.0f },
        { 16, 1.0f },
        { 17, 1.0f },
        { 18, 0.020f },
        { 81, std::string("followGame") },
        { 82, 0u },
        { 83, 0u },
        { 84, 0u },
        { 85, 1.0f },
        { 99, std::string("none") },
      };
      static const preset_bundle_t competitive_bundle {
        { 3, 0.60f },
        { 13, false },
        { 14, 1.0f },
        { 15, 1.0f },
        { 16, 1.0f },
        { 17, 1.0f },
        { 18, 0.020f },
        { 81, std::string("fixed") },
        { 82, 0u },
        { 83, 120u },
        { 84, 255u },
        { 85, 0.35f },
        { 99, std::string("none") },
      };
      static const preset_bundle_t immersive_bundle {
        { 3, 1.40f },
        { 13, true },
        { 14, 1.30f },
        { 15, 1.0f },
        { 16, 1.0f },
        { 17, 1.0f },
        { 18, 0.020f },
        { 81, std::string("followGame") },
        { 82, 0u },
        { 83, 0u },
        { 84, 0u },
        { 85, 1.0f },
        { 99, std::string("none") },
      };
      static const preset_bundle_t legacy_bundle {
        { 3, 0.0f },
        { 13, true },
        { 14, 1.0f },
        { 15, 0.50f },
        { 16, 1.0f },
        { 17, 1.0f },
        { 18, 0.006f },
        { 81, std::string("followGame") },
        { 82, 0u },
        { 83, 0u },
        { 84, 0u },
        { 85, 1.0f },
        { 99, std::string("legacyAll") },
      };
      static const preset_bundle_t empty_bundle;

      if (name == "default") return default_bundle;
      if (name == "competitive") return competitive_bundle;
      if (name == "immersive") return immersive_bundle;
      if (name == "legacy") return legacy_bundle;
      return empty_bundle;
    }

    bool
    same_value(const value_t &left, const value_t &right) {
      if (left.index() != right.index()) return false;
      if (const auto *value = std::get_if<float>(&left)) {
        return std::fabs(*value - std::get<float>(right)) <= 1e-6f;
      }
      if (const auto *value = std::get_if<std::uint32_t>(&left)) {
        return *value == std::get<std::uint32_t>(right);
      }
      if (const auto *value = std::get_if<bool>(&left)) {
        return *value == std::get<bool>(right);
      }
      return std::get<std::string>(left) == std::get<std::string>(right);
    }

    // The preset is derived from the stored host keys, never stored as a
    // second id.  Any key outside the bundle makes the profile custom.
    std::string
    derived_preset_name(const std::map<std::uint32_t, value_t> &stored) {
      for (const auto name : { "default"sv, "competitive"sv, "immersive"sv, "legacy"sv }) {
        const auto &bundle = preset_bundle(name);
        bool matches = true;
        for (const auto &[id, expected] : bundle) {
          const auto found = stored.find(id);
          if (found == stored.end()) {
            const auto *descriptor = find_descriptor(id);
            if (descriptor == nullptr || !same_value(default_value(*descriptor), expected)) {
              matches = false;
              break;
            }
          }
          else if (!same_value(found->second, expected)) {
            matches = false;
            break;
          }
        }
        if (!matches) continue;

        for (const auto &[id, value] : stored) {
          (void) value;
          const bool in_bundle = std::any_of(bundle.begin(), bundle.end(), [id](const auto &entry) { return entry.first == id; });
          if (!in_bundle) {
            matches = false;
            break;
          }
        }
        if (matches) return std::string(name);
      }
      return "custom";
    }

    void
    publish_values(const std::map<std::uint32_t, value_t> &values, const std::map<std::uint32_t, value_t> &stored) {
      const auto number = [&](std::uint32_t id, float fallback) {
        const auto found = values.find(id);
        if (found == values.end()) return fallback;
        if (const auto *value = std::get_if<float>(&found->second)) return *value;
        if (const auto *value = std::get_if<std::uint32_t>(&found->second)) return static_cast<float>(*value);
        if (const auto *value = std::get_if<bool>(&found->second)) return *value ? 1.0f : 0.0f;
        return fallback;
      };
      const auto boolean = [&](std::uint32_t id, bool fallback) {
        const auto found = values.find(id);
        if (found == values.end()) return fallback;
        if (const auto *value = std::get_if<bool>(&found->second)) return *value;
        return fallback;
      };
      const auto unsigned_value = [&](std::uint32_t id, std::uint32_t fallback) {
        const auto found = values.find(id);
        if (found == values.end()) return fallback;
        if (const auto *value = std::get_if<std::uint32_t>(&found->second)) return *value;
        return fallback;
      };
      const auto enum_value = [&](std::uint32_t id, std::string_view fallback) {
        const auto found = values.find(id);
        if (found == values.end()) return std::string(fallback);
        if (const auto *value = std::get_if<std::string>(&found->second)) return *value;
        return std::string(fallback);
      };
      const auto curve_value = [&](std::uint32_t id) {
        const auto name = enum_value(id, "linear");
        if (name == "enhanced") return 1u;
        if (name == "quadratic") return 2u;
        if (name == "cubic") return 3u;
        if (name == "easeOutQuad") return 4u;
        if (name == "easeOutCubic") return 5u;
        return 0u;
      };

      auto &state = runtime::state();
      state.rumble_enabled.store(boolean(13, true), std::memory_order_release);
      state.rumble_strength.store(number(14, 1.0f), std::memory_order_release);
      state.rumble_curve.store(number(15, 1.0f), std::memory_order_release);
      state.rumble_low_gain.store(number(16, 1.0f), std::memory_order_release);
      state.rumble_high_gain.store(number(17, 1.0f), std::memory_order_release);
      state.rumble_noise_gate.store(number(18, 0.020f), std::memory_order_release);
      state.hd_gain.store(number(3, 1.0f), std::memory_order_release);

      const auto haptics_mode = enum_value(2, "auto");
      state.haptics_mode.store(haptics_mode == "off" ? 3u : 0u, std::memory_order_release);
      state.hd_low_band_db.store(number(4, 0.0f), std::memory_order_release);
      state.hd_high_band_db.store(number(5, 0.0f), std::memory_order_release);
      state.hd_crossover_hz.store(number(6, 160.0f), std::memory_order_release);
      state.hd_noise_gate.store(number(7, 0.002f), std::memory_order_release);
      state.hd_latency_ms.store(unsigned_value(8, 10), std::memory_order_release);
      state.hd_smoothing_ms.store(unsigned_value(9, 2), std::memory_order_release);
      state.ir_intensity.store(number(11, 0.8f), std::memory_order_release);
      state.ir_sharpness.store(number(12, 0.5f), std::memory_order_release);
      state.rumble_hold_ms.store(unsigned_value(19, 60), std::memory_order_release);
      state.rumble_send_interval_ms.store(unsigned_value(20, 20), std::memory_order_release);
      state.rumble_watchdog_ms.store(unsigned_value(21, 100), std::memory_order_release);
      state.rumble_max_duration_ms.store(unsigned_value(22, 5000), std::memory_order_release);
      state.rumble_max_rate_hz.store(unsigned_value(23, 250), std::memory_order_release);

      state.trigger_enabled.store(boolean(24, true), std::memory_order_release);
      state.trigger_strength.store(number(25, 1.0f), std::memory_order_release);
      state.trigger_left_strength.store(number(26, 1.0f), std::memory_order_release);
      state.trigger_right_strength.store(number(27, 1.0f), std::memory_order_release);
      state.trigger_feedback_scale.store(number(28, 1.0f), std::memory_order_release);
      state.trigger_weapon_scale.store(number(29, 1.0f), std::memory_order_release);
      state.trigger_vibration_scale.store(number(30, 1.0f), std::memory_order_release);
      state.trigger_slope_scale.store(number(31, 1.0f), std::memory_order_release);
      const auto off_mode = enum_value(32, "release");
      state.trigger_off_mode.store(off_mode == "clear" ? 1u : (off_mode == "hold" ? 2u : 0u),
        std::memory_order_release);
      state.trigger_raw_passthrough.store(boolean(33, false), std::memory_order_release);
      state.trigger_mode_mask.store(unsigned_value(34, 31u), std::memory_order_release);
      const auto touchpad_mode = enum_value(74, "passthrough");
      const auto touchpad_output = stored.contains(75)          ? enum_value(75, "controller") :
                                   touchpad_mode == "off"       ? "off" :
                                   touchpad_mode == "mouse"     ? "hostPointer" :
                                   touchpad_mode == "precision" ? "precision" :
                                                                  "controller";
      state.touchpad_output.store(
        touchpad_output == "hostPointer" ? 1u :
        touchpad_output == "precision"   ? 2u :
        touchpad_output == "off"         ? 3u :
                                           0u,
        std::memory_order_release);
      state.touchpad_sensitivity.store(number(76, 1.0f), std::memory_order_release);
      state.touchpad_scroll_speed.store(number(77, 1.0f), std::memory_order_release);
      state.touchpad_reverse_scroll.store(boolean(78, false), std::memory_order_release);
      const auto click = enum_value(79, "touchpadClick");
      state.touchpad_click_action.store(
        click == "left" ? 1u : click == "right" ? 2u :
                             click == "none"    ? 3u :
                                                  0u,
        std::memory_order_release);
      const auto two_finger = enum_value(80, "scroll");
      state.touchpad_two_finger.store(
        two_finger == "rightClick" ? 1u : two_finger == "none" ? 2u :
                                                                 0u,
        std::memory_order_release);

      const auto identity = enum_value(106, "auto");
      state.identity_mode.store(
        identity == "xbox360" ? 2u : identity == "ds4" ? 3u :
                                   identity == "ds5"   ? 4u :
                                                         0u,
        std::memory_order_release);

      const auto player_mode = enum_value(86, "followGame");
      state.player_led_mode.store(player_mode == "fixed" ? 1u : player_mode == "off" ? 2u :
                                                                                       0u,
        std::memory_order_release);
      state.player_led_index.store(unsigned_value(87, 0), std::memory_order_release);
      const auto mute_mode = enum_value(88, "followGame");
      state.mute_led_mode.store(mute_mode == "on" ? 1u : mute_mode == "off" ? 2u :
                                                                              0u,
        std::memory_order_release);
      state.speaker_level.store(number(90, 1.0f), std::memory_order_release);
      state.headset_level.store(number(91, 1.0f), std::memory_order_release);
      state.mic_level.store(number(92, 0.8f), std::memory_order_release);
      state.speaker_mute.store(boolean(93, false), std::memory_order_release);
      state.mic_mute.store(boolean(94, false), std::memory_order_release);
      const auto route = enum_value(95, "auto");
      state.audio_route.store(route == "speaker" ? 1u : route == "headset" ? 2u :
                                                                             0u,
        std::memory_order_release);
      const auto mic_route = enum_value(96, "controller");
      state.mic_route.store(mic_route == "host" ? 1u : mic_route == "none" ? 2u :
                                                                             0u,
        std::memory_order_release);
      state.audio_preamp.store(enum_value(97, "0dB") == "+6dB" ? 1u : 0u, std::memory_order_release);

      const auto lightbar_mode = enum_value(81, "followGame");
      state.lightbar_mode.store(lightbar_mode == "fixed" ? 1u : (lightbar_mode == "off" ? 2u : 0u), std::memory_order_release);
      state.lightbar_r.store(unsigned_value(82, 0), std::memory_order_release);
      state.lightbar_g.store(unsigned_value(83, 0), std::memory_order_release);
      state.lightbar_b.store(unsigned_value(84, 0), std::memory_order_release);
      state.lightbar_brightness.store(number(85, 1.0f), std::memory_order_release);
      state.release_on_end.store(boolean(89, true), std::memory_order_release);
      const auto compat = enum_value(99, "none");
      state.compat_preset.store(
        compat == "legacyRumble" ? 1u : compat == "legacyTrigger" ? 2u :
                                      compat == "legacyAll"       ? 3u :
                                                                    0u,
        std::memory_order_release);

      state.stick_shape.store(enum_value(35, "perAxis") == "radial" ? 1u : 0u, std::memory_order_release);
      state.left_stick_deadzone_inner.store(number(36, 0.0f), std::memory_order_release);
      state.left_stick_deadzone_outer.store(number(37, 1.0f), std::memory_order_release);
      state.left_stick_anti_deadzone.store(number(38, 0.0f), std::memory_order_release);
      state.left_stick_sensitivity.store(number(39, 1.0f), std::memory_order_release);
      state.left_stick_curve.store(curve_value(40), std::memory_order_release);
      state.left_stick_invert_x.store(boolean(41, false), std::memory_order_release);
      state.left_stick_invert_y.store(boolean(42, false), std::memory_order_release);
      state.right_stick_deadzone_inner.store(number(43, 0.0f), std::memory_order_release);
      state.right_stick_deadzone_outer.store(number(44, 1.0f), std::memory_order_release);
      state.right_stick_anti_deadzone.store(number(45, 0.0f), std::memory_order_release);
      state.right_stick_sensitivity.store(number(46, 1.0f), std::memory_order_release);
      state.right_stick_curve.store(curve_value(47), std::memory_order_release);
      state.right_stick_invert_x.store(boolean(48, false), std::memory_order_release);
      state.right_stick_invert_y.store(boolean(49, false), std::memory_order_release);
      state.left_trigger_deadzone.store(number(50, 0.0f), std::memory_order_release);
      state.right_trigger_deadzone.store(number(53, 0.0f), std::memory_order_release);
      state.left_trigger_threshold.store(number(51, 0.5f), std::memory_order_release);
      state.left_trigger_as_button.store(boolean(52, false), std::memory_order_release);
      state.right_trigger_threshold.store(number(54, 0.5f), std::memory_order_release);
      state.right_trigger_as_button.store(boolean(55, false), std::memory_order_release);
      state.left_trigger_range_min.store(number(56, 0.0f), std::memory_order_release);
      state.left_trigger_range_max.store(number(57, 1.0f), std::memory_order_release);
      state.left_trigger_anti_deadzone.store(number(58, 0.0f), std::memory_order_release);
      state.left_trigger_curve.store(curve_value(59), std::memory_order_release);
      state.right_trigger_range_min.store(number(60, 0.0f), std::memory_order_release);
      state.right_trigger_range_max.store(number(61, 1.0f), std::memory_order_release);
      state.right_trigger_anti_deadzone.store(number(62, 0.0f), std::memory_order_release);
      state.right_trigger_curve.store(curve_value(63), std::memory_order_release);
      const auto activation = enum_value(64, "hostRequested");
      state.gyro_activation.store(activation == "off" ? 0u : 1u, std::memory_order_release);
      state.gyro_sensitivity.store(number(65, 1.0f), std::memory_order_release);
      state.gyro_deadzone.store(number(66, 0.01f), std::memory_order_release);
      const auto filter = enum_value(67, "lowPass");
      state.gyro_filter.store(filter == "none" ? 0u : filter == "oneEuro" ? 2u :
                                                                            1u,
        std::memory_order_release);
      state.gyro_filter_hz.store(number(68, 30.0f), std::memory_order_release);
      state.gyro_filter_beta.store(number(69, 0.05f), std::memory_order_release);
      const auto orientation = enum_value(70, "device");
      state.gyro_orientation.store(orientation == "screen" ? 1u : orientation == "world" ? 2u :
                                                                orientation == "custom"  ? 3u :
                                                                                           0u,
        std::memory_order_release);
      state.gyro_yaw_offset_deg.store(number(71, 0.0f), std::memory_order_release);
      state.gyro_pitch_offset_deg.store(number(72, 0.0f), std::memory_order_release);
      state.gyro_roll_offset_deg.store(number(73, 0.0f), std::memory_order_release);
      runtime::notify_published();
    }

    bool
    parse_value(
      const descriptor_t &descriptor,
      const nlohmann::json &input,
      value_t &output,
      bool &clamped,
      std::string &error) {
      clamped = false;
      switch (descriptor.type) {
        case ValueType::boolean:
          if (!input.is_boolean()) {
            error = "expected a boolean";
            return false;
          }
          output = input.get<bool>();
          return true;
        case ValueType::u32:
        case ValueType::i32: {
          if (!input.is_number_integer()) {
            error = "expected an integer";
            return false;
          }
          const auto raw = input.get<std::int64_t>();
          const auto low = static_cast<std::int64_t>(std::ceil(descriptor.min_value));
          const auto high = static_cast<std::int64_t>(std::floor(descriptor.max_value));
          auto value = std::clamp<std::int64_t>(raw, low, high);
          clamped = value != raw;
          if (descriptor.type == ValueType::u32) {
            if (value < 0) value = 0;
            output = static_cast<std::uint32_t>(value);
          }
          else {
            output = static_cast<std::uint32_t>(value);
          }
          return true;
        }
        case ValueType::f32: {
          if (!input.is_number()) {
            error = "expected a finite number";
            return false;
          }
          const auto raw = input.get<double>();
          if (!std::isfinite(raw)) {
            error = "expected a finite number";
            return false;
          }
          const auto value = std::clamp(raw, descriptor.min_value, descriptor.max_value);
          clamped = value != raw;
          output = static_cast<float>(value);
          return true;
        }
        case ValueType::enumeration: {
          if (!input.is_string()) {
            error = "expected an enum string";
            return false;
          }
          const auto text = input.get<std::string>();
          for (std::uint32_t index = 0; index < descriptor.enum_count; ++index) {
            if (text == descriptor.enum_names[index]) {
              output = text;
              return true;
            }
          }
          error = "unknown enum value '" + text + "'";
          return false;
        }
      }
      error = "unsupported descriptor type";
      return false;
    }

    void
    add_descriptor_json(nlohmann::json &out, const descriptor_t &descriptor, const value_t &value,
      bool stored, bool implemented, bool active) {
      out["id"] = descriptor.id;
      out["key"] = descriptor.key;
      out["label"] = human_label(descriptor);
      out["type"] = value_type_name(descriptor.type);
      out["group"] = static_cast<std::uint32_t>(descriptor.group);
      out["group_name"] = group_name(descriptor.group);
      out["control"] = control_name(descriptor.control);
      out["tier"] = tier_name(descriptor.tier);
      out["scale"] = descriptor.scale == Scale::log ? "log" : "linear";
      out["authority"] = authority_name(descriptor.authority);
      out["stage"] = static_cast<std::uint32_t>(descriptor.stage);
      out["live"] = static_cast<std::uint32_t>(descriptor.live);
      out["expert"] = (descriptor.flags & FLAG_EXPERT) != 0;
      out["persisted"] = (descriptor.flags & FLAG_PERSISTED) != 0;
      out["min"] = descriptor.min_value;
      out["max"] = descriptor.max_value;
      out["step"] = descriptor.step;
      out["default"] = value_to_json(default_value(descriptor));
      out["value"] = value_to_json(value);
      out["stored"] = stored;
      out["source"] = stored ? "host" : "default";
      out["implemented"] = implemented;
      out["active"] = active;
      out["status"] = !implemented ? "unavailable" : (active ? "applied" : "ready");
      out["reason"] = !implemented ? wire_reason(descriptor.id) :
                                     (active ? std::string {} : std::string { "Native DualSense path is idle; the saved value applies on the next native feedback." });
      out["note"] = wire_note(descriptor.id);
      out["unit"] = descriptor.unit ? nlohmann::json(descriptor.unit) : nlohmann::json(nullptr);
      out["legacy_key"] = descriptor.legacy_key ? nlohmann::json(descriptor.legacy_key) : nlohmann::json(nullptr);
      out["title_i18n_key"] = descriptor.title_i18n_key;
      out["description_i18n_key"] = descriptor.description_i18n_key;
      out["enum_names"] = nlohmann::json::array();
      for (std::uint32_t index = 0; index < descriptor.enum_count; ++index) {
        out["enum_names"].push_back(descriptor.enum_names[index]);
      }
    }
  }  // namespace

  const std::vector<descriptor_t> &
  descriptors() {
    return table();
  }

  const descriptor_t *
  find_descriptor(std::uint32_t id) {
    const auto found = by_id().find(id);
    return found == by_id().end() ? nullptr : found->second;
  }

  const descriptor_t *
  find_descriptor(std::string_view key) {
    const auto found = by_key().find(key);
    return found == by_key().end() ? nullptr : found->second;
  }

  value_t
  default_value(const descriptor_t &descriptor) {
    switch (descriptor.type) {
      case ValueType::boolean:
        return descriptor.default_number != 0.0;
      case ValueType::u32:
      case ValueType::i32:
        return static_cast<std::uint32_t>(descriptor.default_number);
      case ValueType::f32:
        return static_cast<float>(descriptor.default_number);
      case ValueType::enumeration:
        return std::string(descriptor.default_enum ? descriptor.default_enum : "");
    }
    return std::string {};
  }

  std::string
  group_name(Group group) {
    switch (group) {
      case Group::general:
        return "General";
      case Group::haptics:
        return "Haptics";
      case Group::rumble:
        return "Rumble";
      case Group::adaptive_trigger:
        return "Adaptive triggers";
      case Group::sticks:
        return "Sticks";
      case Group::trigger_axes:
        return "Trigger axes";
      case Group::motion:
        return "Motion";
      case Group::touchpad:
        return "Touchpad";
      case Group::lighting:
        return "Lighting";
      case Group::audio:
        return "Audio";
      case Group::compatibility:
        return "Compatibility";
      case Group::sync:
        return "Sync";
      case Group::routing:
        return "Routing";
    }
    return "General";
  }

  std::string
  control_name(Control control) {
    switch (control) {
      case Control::slider:
        return "slider";
      case Control::toggle:
        return "toggle";
      case Control::segmented:
        return "segmented";
      case Control::menu:
        return "menu";
      case Control::color_channel:
        return "color_channel";
      case Control::curve:
        return "curve";
      case Control::chip_multiselect:
        return "chip_multiselect";
      case Control::preset_grid:
        return "preset_grid";
    }
    return "menu";
  }

  std::string
  tier_name(Tier tier) {
    return tier == Tier::common ? "common" : "advanced";
  }

  std::string
  authority_name(Authority authority) {
    switch (authority) {
      case Authority::device:
        return "device";
      case Authority::host:
        return "host";
      case Authority::client:
        return "client";
      case Authority::shared_host:
        return "shared_host";
      case Authority::shared_client:
        return "shared_client";
      case Authority::product:
        return "product";
    }
    return "host";
  }

  std::string
  wire_reason(std::uint32_t id) {
    if (runtime::key_implemented(id)) return {};
    if (id == 10) {
      return "Bluetooth output fallback is chosen by the client from its local audio route; this host bridge has no BT sink.";
    }
    if (id == 31) {
      return "The pad has no slope effect type: a slope is sent as a ten-zone 0x21 block, which the host cannot tell from multi-position feedback, so no block can be scaled as slope without guessing.";
    }
    if (id == 105) {
      return "Feedback target routing is a client sink decision.";
    }
    if (!runtime::native_path_compiled()) {
      return "The native DualSense feedback path is not compiled on this platform.";
    }
    if (id >= 4 && id <= 12) {
      return "The native bridge does not shape HD haptics bands, crossover, latency, BT fallback, or IR.";
    }
    if (id >= 19 && id <= 23) {
      return "The native bridge does not merge, hold, watchdog, or rate-cap feedback.";
    }
    if (id >= 24 && id <= 34) {
      return "The native bridge forwards game-authored trigger blocks; per-mode strength scaling is not implemented.";
    }
    if (id >= 74 && id <= 80) {
      return "Needs a host pointer injector outside the HID report path (move, button, wheel, or a precision touchpad). This bridge only copies the contact into the controller report.";
    }
    if (id >= 35 && id <= 63) {
      return "Stick and trigger-axis shaping runs in the client input path, not in this host feedback bridge.";
    }
    if (id >= 64 && id <= 73) return "Motion filtering and orientation run in the client input path.";
    if (id == 96) {
      return "The DualSense microphone endpoint has no capture source on this host yet (it answers silence); the virtual microphone is a separate task, so there is no route to choose.";
    }
    if (id == 97) {
      return "The 0x5515 output extension carries no audio_control2 field, so the speaker preamp cannot reach the client.";
    }
    if (id == 98 || id == 100) {
      return "Bluetooth audio and output path are client radio decisions; this host bridge is USB only. The macOS client implements this key.";
    }
    if (id == 101) {
      return "The cap limits client sampling of the physical pad. The host accepts pushed reports and has no polling loop to cap.";
    }
    if (id >= 102 && id <= 104) {
      return "Needs an ALK-DS5T/1 peer with HELLO/PATCH/SNAPSHOT/ACK and the section 16.3 layer merge; this build has no tuning control channel for the override gates or rate cap.";
    }
    switch (id) {
      case 2:
        return "Audio-haptics routing is selected when the native controller is allocated; this build has no live switch.";
      case 105:
        return "Feedback target routing is a client sink decision.";
      default:
        return "No native consumer is wired for this key in this build.";
    }
  }

  std::string
  wire_note(std::uint32_t id) {
    switch (id) {
      case 1:
        return "Writes the named host bundle; a later key change derives the preset as custom.";
      case 3:
        return "Applies to the next PCM haptics frame.";
      case 13:
        return "Zero is written on the next valid rumble report; there is no independent watchdog in this build.";
      case 14:
      case 15:
      case 16:
      case 17:
      case 18:
        return "Applied on the next valid game feedback report.";
      case 81:
        return "Fixed/off overrides the next game lightbar update.";
      case 82:
      case 83:
      case 84:
      case 85:
        return "Applied on the next game lightbar update.";
      case 89:
        return "Session end steps the last lightbar colour toward black on set_rgb_led. Player and mute LEDs are not part of this path.";
      case 74:
      case 75:
        return "Applies to the next controller touch. Host pointer uses pointer motion; precision uses the platform synthetic touchpad and falls back to the virtual controller when that capability is absent. An explicit output value takes precedence over the legacy touchpad mode.";
      case 76:
      case 77:
      case 78:
        return "Applies to the next host-pointer gesture.";
      case 79:
        return "Applies to the next touchpad button transition.";
      case 80:
        return "Applies when two contacts are down.";
      case 99:
        return "legacyRumble and legacyAll drop the next authored PCM frame. Every legacy preset sends the next adaptive trigger as release 0x05.";
      case 51:
      case 54:
        return "Sets the raw trigger level at which the digital L2/R2 report bit turns on.";
      case 52:
      case 55:
        return "The analog axis becomes 0 or 255 by the threshold decision; the digital bit is set in either mode.";
      case 106:
        return "Applies to the next virtual controller allocation; active devices keep their current identity.";
      default:
        return {};
    }
  }

  std::filesystem::path
  profile_path_for(std::string_view config_file, const std::filesystem::path &appdata) {
    if (!config_file.empty()) {
      const std::filesystem::path configured(config_file);
      if (!configured.parent_path().empty()) {
        return configured.parent_path() / "dualsense_tuning.json";
      }
    }
    return appdata / "dualsense_tuning.json";
  }

  std::string
  human_label(const descriptor_t &descriptor) {
    std::string text(descriptor.key);
    for (const auto prefix : { "ds5."sv, "gamepad."sv }) {
      if (text.starts_with(prefix)) {
        text.erase(0, prefix.size());
        break;
      }
    }
    std::string spaced;
    spaced.reserve(text.size() * 2);
    for (std::size_t index = 0; index < text.size(); ++index) {
      const char character = text[index];
      if (character == '.') {
        spaced.push_back(' ');
        continue;
      }
      if (index && std::isupper(static_cast<unsigned char>(character)) &&
          (std::islower(static_cast<unsigned char>(text[index - 1])) ||
            std::isdigit(static_cast<unsigned char>(text[index - 1])))) {
        spaced.push_back(' ');
      }
      spaced.push_back(character);
    }
    for (const auto &[from, to] : std::initializer_list<std::pair<std::string_view, std::string_view>> {
           { "Led", "LED" }, { "Hd", "HD" }, { "Ir", "IR" }, { "Bt", "BT" },
           { "Db", "dB" }, { "Hz", "Hz" }, { "Ms", "ms" }, { "Deg", "deg" } }) {
      std::size_t position = 0;
      while ((position = spaced.find(from, position)) != std::string::npos) {
        spaced.replace(position, from.size(), to);
        position += to.size();
      }
    }
    const auto replace_word = [&spaced](std::string_view from, std::string_view to) {
      std::size_t position = 0;
      while ((position = spaced.find(from, position)) != std::string::npos) {
        const bool start = position == 0 || spaced[position - 1] == ' ';
        const bool end = position + from.size() == spaced.size() || spaced[position + from.size()] == ' ';
        if (start && end) {
          spaced.replace(position, from.size(), to);
          position += to.size();
        }
        else {
          position += from.size();
        }
      }
    };
    for (const auto &[from, to] : std::initializer_list<std::pair<std::string_view, std::string_view>> {
           { "hd", "HD" }, { "led", "LED" }, { "ir", "IR" }, { "bt", "BT" } }) {
      replace_word(from, to);
    }
    if (!spaced.empty()) spaced[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(spaced[0])));
    return spaced;
  }

  store_t::store_t(std::filesystem::path path): path_(std::move(path)) {}

  std::uint64_t
  store_t::revision() const {
    std::lock_guard lock(mutex_);
    return revision_;
  }

  std::map<std::uint32_t, value_t>
  store_t::effective_locked() const {
    std::map<std::uint32_t, value_t> result;
    for (const auto &descriptor : descriptors()) result.emplace(descriptor.id, default_value(descriptor));
    for (const auto &[id, value] : values_) {
      if (runtime::key_implemented(id)) result[id] = value;
    }
    return result;
  }

  nlohmann::json
  store_t::snapshot_locked() const {
    const auto effective = effective_locked();
    nlohmann::json out;
    out["schema"] = "ALK-DS5T/1";
    out["status"] = true;
    out["revision"] = revision_;
    out["keys"] = nlohmann::json::array();
    out["common_keys"] = nlohmann::json::array();
    std::uint32_t implemented_count = 0;
    std::uint32_t wired_count = 0;
    const bool active = runtime::native_path_active();
    for (const auto &descriptor : descriptors()) {
      const auto found = effective.find(descriptor.id);
      auto value = found == effective.end() ? default_value(descriptor) : found->second;
      if (descriptor.id == 1) value = derived_preset_name(values_);
      const bool implemented = runtime::key_implemented(descriptor.id);
      if (implemented) ++implemented_count;
      if (descriptor.tier == Tier::common) out["common_keys"].push_back(descriptor.id);
      nlohmann::json entry;
      add_descriptor_json(entry, descriptor, value, descriptor.id != 1 && values_.contains(descriptor.id), implemented, implemented && active);
      out["keys"].push_back(std::move(entry));
    }
    wired_count = implemented_count;
    out["implemented_count"] = implemented_count;
    out["wired_count"] = wired_count;
    out["active_count"] = active ? 1 : 0;
    out["native_path_compiled"] = runtime::native_path_compiled();
    if (!load_error_.empty()) out["load_error"] = load_error_;
    if (!load_warnings_.empty()) out["load_warnings"] = load_warnings_;
    return out;
  }

  nlohmann::json
  store_t::snapshot_json() const {
    std::lock_guard lock(mutex_);
    return snapshot_locked();
  }

  void
  store_t::publish() const {
    std::map<std::uint32_t, value_t> effective;
    std::map<std::uint32_t, value_t> stored;
    {
      std::lock_guard lock(mutex_);
      effective = effective_locked();
      stored = values_;
    }
    publish_values(effective, stored);
  }

  void
  store_t::migrate_legacy_locked() {
    /* The earlier host kept four values in ds5_config.json beside sunshine.conf; the
     * file is only read, never changed or removed.  ds5_genshin_compatibility has no
     * successor: the product string is fixed. */
    struct legacy_t {
      const char *legacy_key;
      const char *key;
    };
    static constexpr legacy_t table[] = {
      { "ds5_audio_haptics", "ds5.haptics.mode" },
      { "ds5_legacy_haptics_strength", "ds5.rumble.strength" },
      { "ds5_legacy_haptics_curve", "ds5.rumble.curve" },
      { "ds5_legacy_haptics_noise_gate", "ds5.rumble.noiseGate" },
    };
    const auto directory = path_.parent_path();
    std::map<std::string, nlohmann::json> found;

    std::error_code ignored;
    const auto conf = directory / "sunshine.conf";
    if (std::filesystem::exists(conf, ignored)) {
      std::ifstream input(conf);
      std::string line;
      while (std::getline(input, line)) {
        const auto equals = line.find('=');
        if (equals == std::string::npos) continue;
        const auto trim = [](std::string text) {
          const auto first = text.find_first_not_of(" \t\r\n");
          if (first == std::string::npos) return std::string {};
          return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
        };
        const auto name = trim(line.substr(0, equals));
        const auto text = trim(line.substr(equals + 1));
        for (const auto &entry : table) {
          if (name != entry.legacy_key) continue;
          if (text == "true" || text == "enabled" || text == "on") found[name] = true;
          else if (text == "false" || text == "disabled" || text == "off") found[name] = false;
          else {
            try {
              std::size_t used = 0;
              const auto number = std::stod(text, &used);
              if (used == text.size()) found[name] = number;
            }
            catch (...) {
            }
          }
        }
      }
    }

    const auto legacy_file = directory / "ds5_config.json";
    if (std::filesystem::exists(legacy_file, ignored) && std::filesystem::file_size(legacy_file, ignored) <= 64 * 1024) {
      try {
        std::ifstream input(legacy_file);
        nlohmann::json root;
        input >> root;
        if (root.is_object()) {
          for (const auto &entry : table) {
            const auto item = root.find(entry.legacy_key);
            if (item != root.end() && (item->is_number() || item->is_boolean())) found[entry.legacy_key] = *item;  // the file wins over sunshine.conf
          }
        }
      }
      catch (const std::exception &exception) {
        load_warnings_.push_back(std::string("ignored ds5_config.json: ") + exception.what());
      }
    }

    for (const auto &entry : table) {
      const auto item = found.find(entry.legacy_key);
      if (item == found.end()) continue;
      const auto *descriptor = find_descriptor(entry.key);
      if (!descriptor || !runtime::key_implemented(descriptor->id)) continue;
      nlohmann::json input = item->second;
      if (descriptor->id == 2) {
        if (!input.is_boolean()) continue;
        input = input.get<bool>() ? "auto" : "off";
      }
      value_t parsed;
      bool clamped = false;
      std::string parse_error;
      if (!parse_value(*descriptor, input, parsed, clamped, parse_error)) {
        load_warnings_.push_back(std::string("did not migrate ") + entry.legacy_key + ": " + parse_error);
        continue;
      }
      values_[descriptor->id] = std::move(parsed);
    }
    if (values_.empty()) return;
    revision_ = 1;
    std::string save_error;
    if (!save_locked(values_, revision_, &save_error)) {
      load_warnings_.push_back("migrated legacy values are applied but could not be saved: " + save_error);
    }
  }

  bool
  store_t::load(std::string *error) {
    std::map<std::uint32_t, value_t> effective;
    std::map<std::uint32_t, value_t> stored;
    {
      std::lock_guard lock(mutex_);
      values_.clear();
      load_warnings_.clear();
      load_error_.clear();
      revision_ = 0;

      std::error_code file_error;
      const bool exists = std::filesystem::exists(path_, file_error);
      if (file_error) {
        load_error_ = "cannot inspect " + path_.string() + ": " + file_error.message();
        if (error) *error = load_error_;
        return false;
      }
      if (!exists) {
        migrate_legacy_locked();
        effective = effective_locked();
      }
      else {
        std::ifstream input(path_, std::ios::binary);
        if (!input) {
          load_error_ = "cannot open " + path_.string();
          if (error) *error = load_error_;
          return false;
        }
        try {
          nlohmann::json root;
          input >> root;
          if (!root.is_object() || root.value("schema", "") != "ALK-DS5T/1") {
            throw std::runtime_error("schema is not ALK-DS5T/1");
          }
          if (root.contains("revision") && root["revision"].is_number_unsigned()) {
            revision_ = root["revision"].get<std::uint64_t>();
          }
          const auto values = root.find("values");
          if (values != root.end()) {
            if (!values->is_object()) throw std::runtime_error("values is not an object");
            for (const auto &[key, input_value] : values->items()) {
              const auto *descriptor = find_descriptor(key);
              if (!descriptor) {
                load_warnings_.push_back("ignored unknown key " + key);
                continue;
              }
              value_t parsed;
              bool clamped = false;
              std::string parse_error;
              if (!parse_value(*descriptor, input_value, parsed, clamped, parse_error)) {
                load_warnings_.push_back("ignored " + key + ": " + parse_error);
                continue;
              }
              if (!runtime::key_implemented(descriptor->id)) {
                load_warnings_.push_back("ignored " + key + ": " + wire_reason(descriptor->id));
                continue;
              }
              if (descriptor->id == 1) {
                // The preset is derived; a stored copy would be a second id.
                continue;
              }
              if (clamped) load_warnings_.push_back("clamped " + key + " to the descriptor range");
              values_[descriptor->id] = std::move(parsed);
            }
          }
        }
        catch (const std::exception &exception) {
          load_error_ = "cannot read " + path_.string() + ": " + exception.what();
          if (error) *error = load_error_;
          return false;
        }
        effective = effective_locked();
        stored = values_;
      }
    }
    publish_values(effective, stored);
    return true;
  }

  bool
  store_t::save_locked(const std::map<std::uint32_t, value_t> &values, std::uint64_t revision, std::string *error) {
    std::error_code directory_error;
    if (!path_.parent_path().empty()) {
      std::filesystem::create_directories(path_.parent_path(), directory_error);
      if (directory_error) {
        if (error) *error = "cannot create " + path_.parent_path().string() + ": " + directory_error.message();
        return false;
      }
    }

    nlohmann::json root;
    root["schema"] = "ALK-DS5T/1";
    root["revision"] = revision;
    root["values"] = nlohmann::json::object();
    for (const auto &descriptor : descriptors()) {
      if (descriptor.id == 1) continue;
      const auto found = values.find(descriptor.id);
      if (found != values.end()) root["values"][descriptor.key] = value_to_json(found->second);
    }

    const auto temporary = path_.string() + ".tmp";
    {
      std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
      if (!output) {
        if (error) *error = "cannot open " + temporary;
        return false;
      }
      output << root.dump(2) << '\n';
      output.flush();
      if (!output) {
        if (error) *error = "cannot write " + temporary;
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
      }
    }

    std::error_code rename_error;
    std::filesystem::rename(temporary, path_, rename_error);
    if (rename_error) {
      // Windows does not replace an existing file through std::filesystem::rename.
      std::error_code remove_error;
      std::filesystem::remove(path_, remove_error);
      rename_error.clear();
      std::filesystem::rename(temporary, path_, rename_error);
    }
    if (rename_error) {
      if (error) *error = "cannot replace " + path_.string() + ": " + rename_error.message();
      std::error_code ignored;
      std::filesystem::remove(temporary, ignored);
      return false;
    }
    return true;
  }

  patch_result_t
  store_t::apply_json(const nlohmann::json &body) {
    patch_result_t result;
    std::map<std::uint32_t, value_t> effective;
    std::map<std::uint32_t, value_t> stored;
    {
      std::lock_guard lock(mutex_);
      if (!body.is_object()) {
        result.error = "request body must be a JSON object";
        result.code = 9;
        result.http_status = 400;
        return result;
      }
      if (body.value("schema", "") != "ALK-DS5T/1") {
        result.error = "schema must be ALK-DS5T/1";
        result.code = 9;
        result.http_status = 400;
        return result;
      }
      const auto values = body.find("values");
      if (values == body.end() || !values->is_object() || values->empty()) {
        result.error = "values must be a non-empty object";
        result.code = 9;
        result.http_status = 400;
        return result;
      }
      if (values->size() > descriptors().size()) {
        result.error = "too many values";
        result.code = 9;
        result.http_status = 400;
        return result;
      }

      std::map<std::uint32_t, value_t> candidate;
      nlohmann::json applied = nlohmann::json::array();
      nlohmann::json unavailable = nlohmann::json::array();
      for (const auto &[key, input] : values->items()) {
        const auto *descriptor = find_descriptor(key);
        if (!descriptor) {
          result.error = "unknown key '" + key + "'";
          result.code = 9;
          result.http_status = 400;
          return result;
        }
        value_t parsed;
        bool clamped = false;
        std::string parse_error;
        bool parsed_ok = false;
        try {
          parsed_ok = parse_value(*descriptor, input, parsed, clamped, parse_error);
        }
        catch (const std::exception &exception) {
          result.error = key + ": " + exception.what();
          result.code = 9;
          result.http_status = 400;
          return result;
        }
        if (!parsed_ok) {
          result.error = key + ": " + parse_error;
          result.code = 3;
          result.http_status = 400;
          return result;
        }
        if (descriptor->id == 1) {
          const auto preset = std::get<std::string>(parsed);
          const auto &bundle = preset_bundle(preset);
          if (bundle.empty()) {
            result.error = preset == "custom" ?
                             "custom keeps the current values; submit the keys to change" :
                             "preset '" + preset + "' has no host bundle";
            result.code = 3;
            result.http_status = 400;
            return result;
          }
          for (const auto &[id, expanded] : bundle) {
            const auto *target = find_descriptor(id);
            if (target == nullptr) continue;
            // An explicit value in the same request wins over the bundle.
            if (values->contains(target->key)) continue;
            if (!runtime::key_implemented(id)) {
              unavailable.push_back({
                { "id", id },
                { "key", target->key },
                { "code", 5 },
                { "reason", wire_reason(id) },
              });
              continue;
            }
            candidate[id] = expanded;
            applied.push_back({
              { "id", id },
              { "key", target->key },
              { "value", value_to_json(expanded) },
              { "clamped", false },
              { "preset", preset },
            });
          }
          continue;
        }
        if (!runtime::key_implemented(descriptor->id)) {
          unavailable.push_back({
            { "id", descriptor->id },
            { "key", descriptor->key },
            { "code", 5 },
            { "reason", wire_reason(descriptor->id) },
          });
          continue;
        }
        candidate[descriptor->id] = parsed;
        applied.push_back({
          { "id", descriptor->id },
          { "key", descriptor->key },
          { "value", value_to_json(parsed) },
          { "clamped", clamped },
        });
      }

      if (candidate.empty()) {
        result.body = snapshot_locked();
        result.body["status"] = false;
        result.body["code"] = 5;
        result.body["error"] = "none of the submitted keys are implemented by the running path";
        result.body["applied"] = applied;
        result.body["unavailable"] = unavailable;
        result.error = result.body["error"];
        result.code = 5;
        result.http_status = 409;
        return result;
      }

      auto next_values = values_;
      for (const auto &[id, value] : candidate) next_values[id] = value;
      const auto next_revision = revision_ + 1;
      std::string save_error;
      if (!save_locked(next_values, next_revision, &save_error)) {
        result.body = snapshot_locked();
        result.body["status"] = false;
        result.body["code"] = 8;
        result.body["error"] = save_error;
        result.error = save_error;
        result.code = 8;
        result.http_status = 500;
        return result;
      }
      values_ = std::move(next_values);
      revision_ = next_revision;
      effective = effective_locked();
      stored = values_;
      result.body = snapshot_locked();
      result.body["status"] = true;
      result.body["partial"] = !unavailable.empty();
      result.body["code"] = unavailable.empty() ? 0 : 1;
      result.body["applied"] = applied;
      result.body["unavailable"] = unavailable;
      result.ok = true;
      result.code = unavailable.empty() ? 0 : 1;
      result.http_status = 200;
    }
    publish_values(effective, stored);
    return result;
  }
}  // namespace ds5::tuning
