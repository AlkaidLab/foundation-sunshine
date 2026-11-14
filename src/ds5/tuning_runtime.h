/**
 * @file src/ds5/tuning_runtime.h
 * @brief Process-local snapshot of the DS5 tuning values the native paths can apply.
 *
 * The profile schema and its persistence belong to tuning_profile.h.  This
 * header is deliberately small and allocation-free: the Windows USB/IP
 * feedback path and the host-side authored-haptics reduction read the atomics
 * on their report threads, while the Web UI thread publishes a new snapshot
 * after a validated save.
 *
 * Key ids follow ALK-DS5T/1, docs/architecture/ds5-platform-design.zh.md
 * sections 16.4.11-16.4.12 on branch docs/ds5-tuning-profile (commit
 * 29f7673fb, read 2026-10-31).  Only keys whose values change a running path
 * report an implementation; everything else is surfaced as UNAVAILABLE
 * instead of being stored and silently ignored.
 */
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "src/ds5/control_extension.h"
#include "src/ds5/touchpad_mapper.h"

namespace ds5::tuning::runtime {
  struct feedback_state_t {
    std::atomic<bool> rumble_enabled { true };
    std::atomic<float> rumble_strength { 1.0f };
    std::atomic<float> rumble_low_gain { 1.0f };
    std::atomic<float> rumble_high_gain { 1.0f };
    std::atomic<float> rumble_curve { 1.0f };
    std::atomic<float> rumble_noise_gate { 0.020f };
    std::atomic<float> hd_gain { 1.0f };

    // 0 auto, 3 off. The route (raw PCM or analysed IR) follows what the client offers, so no other value exists.
    std::atomic<std::uint32_t> haptics_mode { 0 };
    std::atomic<float> hd_low_band_db { 0.0f };
    std::atomic<float> hd_high_band_db { 0.0f };
    std::atomic<float> hd_crossover_hz { 160.0f };
    std::atomic<float> hd_noise_gate { 0.002f };
    std::atomic<std::uint32_t> hd_latency_ms { 10 };
    std::atomic<std::uint32_t> hd_smoothing_ms { 2 };
    std::atomic<float> ir_intensity { 0.8f };
    std::atomic<float> ir_sharpness { 0.5f };

    std::atomic<std::uint32_t> rumble_hold_ms { 60 };
    std::atomic<std::uint32_t> rumble_send_interval_ms { 20 };
    std::atomic<std::uint32_t> rumble_watchdog_ms { 100 };
    std::atomic<std::uint32_t> rumble_max_duration_ms { 5000 };
    std::atomic<std::uint32_t> rumble_max_rate_hz { 250 };

    std::atomic<bool> trigger_enabled { true };
    std::atomic<float> trigger_strength { 1.0f };
    std::atomic<float> trigger_left_strength { 1.0f };
    std::atomic<float> trigger_right_strength { 1.0f };
    std::atomic<float> trigger_feedback_scale { 1.0f };
    std::atomic<float> trigger_weapon_scale { 1.0f };
    std::atomic<float> trigger_vibration_scale { 1.0f };
    std::atomic<float> trigger_slope_scale { 1.0f };
    // 0 release, 1 clear, 2 hold.
    std::atomic<std::uint32_t> trigger_off_mode { 0 };
    std::atomic<bool> trigger_raw_passthrough { false };
    std::atomic<std::uint32_t> trigger_mode_mask { 31u };

    // 0 follows the game, 1 uses the fixed colour, 2 turns the lightbar off.
    std::atomic<std::uint32_t> lightbar_mode { 0 };
    std::atomic<std::uint32_t> lightbar_r { 0 };
    std::atomic<std::uint32_t> lightbar_g { 0 };
    std::atomic<std::uint32_t> lightbar_b { 0 };
    std::atomic<float> lightbar_brightness { 1.0f };

    // Last colour apply_lightbar emitted.  Session release fades this toward black.
    std::atomic<std::uint8_t> lightbar_out_r { 0 };
    std::atomic<std::uint8_t> lightbar_out_g { 0 };
    std::atomic<std::uint8_t> lightbar_out_b { 0 };
    std::atomic<bool> release_on_end { true };

    // 0 none, 1 legacyRumble, 2 legacyTrigger, 3 legacyAll.
    std::atomic<std::uint32_t> compat_preset { 0 };

    // Input shaping is read by the native report builder.  The values are
    // published by tuning_profile.cpp after validation; no report-path lock
    // or allocation is needed.
    std::atomic<std::uint32_t> stick_shape { 0 };
    std::atomic<float> left_stick_deadzone_inner { 0.0f };
    std::atomic<float> left_stick_deadzone_outer { 1.0f };
    std::atomic<float> left_stick_anti_deadzone { 0.0f };
    std::atomic<float> left_stick_sensitivity { 1.0f };
    std::atomic<std::uint32_t> left_stick_curve { 0 };
    std::atomic<bool> left_stick_invert_x { false };
    std::atomic<bool> left_stick_invert_y { false };
    std::atomic<float> right_stick_deadzone_inner { 0.0f };
    std::atomic<float> right_stick_deadzone_outer { 1.0f };
    std::atomic<float> right_stick_anti_deadzone { 0.0f };
    std::atomic<float> right_stick_sensitivity { 1.0f };
    std::atomic<std::uint32_t> right_stick_curve { 0 };
    std::atomic<bool> right_stick_invert_x { false };
    std::atomic<bool> right_stick_invert_y { false };
    std::atomic<float> left_trigger_deadzone { 0.0f };
    std::atomic<float> left_trigger_threshold { 0.5f };
    std::atomic<float> right_trigger_threshold { 0.5f };
    std::atomic<bool> left_trigger_as_button { false };
    std::atomic<bool> right_trigger_as_button { false };
    std::atomic<float> right_trigger_deadzone { 0.0f };
    std::atomic<float> left_trigger_range_min { 0.0f };
    std::atomic<float> left_trigger_range_max { 1.0f };
    std::atomic<float> left_trigger_anti_deadzone { 0.0f };
    std::atomic<std::uint32_t> left_trigger_curve { 0 };
    std::atomic<float> right_trigger_range_min { 0.0f };
    std::atomic<float> right_trigger_range_max { 1.0f };
    std::atomic<float> right_trigger_anti_deadzone { 0.0f };
    std::atomic<std::uint32_t> right_trigger_curve { 0 };
    std::atomic<std::uint32_t> gyro_activation { 1 };
    std::atomic<float> gyro_sensitivity { 1.0f };
    std::atomic<float> gyro_deadzone { 0.01f };
    std::atomic<std::uint32_t> gyro_filter { 1 };
    std::atomic<float> gyro_filter_hz { 30.0f };
    std::atomic<float> gyro_filter_beta { 0.05f };
    std::atomic<std::uint32_t> gyro_orientation { 0 };
    std::atomic<float> gyro_yaw_offset_deg { 0.0f };
    std::atomic<float> gyro_pitch_offset_deg { 0.0f };
    std::atomic<float> gyro_roll_offset_deg { 0.0f };
    // Keys 74-80.  The resolved output is 0 controller, 1 host pointer,
    // 2 precision touchpad, 3 off.
    std::atomic<std::uint32_t> touchpad_output { 0 };
    std::atomic<float> touchpad_sensitivity { 1.0f };
    std::atomic<float> touchpad_scroll_speed { 1.0f };
    std::atomic<bool> touchpad_reverse_scroll { false };
    std::atomic<std::uint32_t> touchpad_click_action { 0 };
    std::atomic<std::uint32_t> touchpad_two_finger { 0 };

    // Key 106: 0 auto, 2 Xbox 360, 3 DualShock 4, 4 DualSense.
    std::atomic<std::uint32_t> identity_mode { 0 };

    /*
     * Values prepared for the ALK-DS5T/1 feedback extension (keys 86-88 and
     * 90-97).  The platform output report bytes are owned by that extension;
     * publishing here keeps the profile and the report writer on one source
     * of truth.  The accessors below are pure and allocation-free.
     */
    std::atomic<std::uint32_t> player_led_mode { 0 };
    std::atomic<std::uint32_t> player_led_index { 0 };
    std::atomic<std::uint32_t> mute_led_mode { 0 };
    std::atomic<float> speaker_level { 1.0f };
    std::atomic<float> headset_level { 1.0f };
    std::atomic<float> mic_level { 0.8f };
    std::atomic<bool> speaker_mute { false };
    std::atomic<bool> mic_mute { false };
    std::atomic<std::uint32_t> audio_route { 0 };
    std::atomic<std::uint32_t> mic_route { 0 };
    std::atomic<std::uint32_t> audio_preamp { 0 };

    /*
     * Number of live native DualSense allocations.  The settings page uses it
     * to distinguish "implemented, waiting for a native controller" from
     * "applied to a running native controller" without inventing a second
     * capability probe.
     */
    std::atomic<std::uint32_t> active_controllers { 0 };
  };

  inline feedback_state_t &
  state() {
    static feedback_state_t value;
    return value;
  }

  inline bool
  native_path_compiled() {
#if defined(DS5_TUNING_TEST_NATIVE_PATH)
    // The validation/persistence unit test compiles the same table on Linux
    // and needs a deterministic native-path capability.  Production builds
    // never define this macro.
    return true;
#elif defined(_WIN32)
    return true;
#else
    return false;
#endif
  }

  /**
   * @brief Whether this build contains the host-side authored-haptics reduction.
   *
   * src/haptics/authored_ir.cpp is in the host source inventory on every
   * platform, so its band, gate, latency, smoothing and IR keys have a live
   * consumer even when the Windows USB/IP bridge is not compiled.
   */
  inline bool
  analysis_path_compiled() {
    return true;
  }

  inline bool
  native_path_active() {
    return state().active_controllers.load(std::memory_order_acquire) != 0;
  }

  /**
   * @brief Whether this build can apply a key to a running path.
   *
   * Adding a key here is the single place that changes its UI status from
   * UNAVAILABLE.  Keys that only the client can execute stay out.
   * These are the keys whose values change host behaviour: HD PCM gain, ERM
   * rumble shaping, lightbar mode/colour/brightness, the session-end lightbar
   * fade, the legacy compatibility preset, the host pointer/precision touchpad
   * mapping, preset expansion and the next-allocation identity profile.
   * Adding a key here is the single place that changes its UI status from
   * UNAVAILABLE.
   */
  inline bool
  key_implemented(std::uint32_t key) {
    // Host-side authored-haptics reduction: band gains, crossover, gate,
    // smoothing, IR intensity/sharpness, and the ERM fallback timing.
    if (analysis_path_compiled()) {
      switch (key) {
        case 2:  // ds5.haptics.mode
        case 4:  // ds5.haptics.hd.lowBandDb
        case 5:  // ds5.haptics.hd.highBandDb
        case 6:  // ds5.haptics.hd.crossoverHz
        case 7:  // ds5.haptics.hd.noiseGate
        case 8:  // ds5.haptics.hd.latencyMs
        case 9:  // ds5.haptics.hd.smoothingMs
        case 11:  // ds5.haptics.ir.intensity
        case 12:  // ds5.haptics.ir.sharpness
        case 19:  // ds5.rumble.holdMs
        case 20:  // ds5.rumble.sendIntervalMs
        case 21:  // ds5.rumble.watchdogMs
        case 22:  // ds5.rumble.maxDurationMs
        case 23:  // ds5.rumble.maxRateHz
          return true;
        default:
          break;
      }
    }
    if (!native_path_compiled()) return false;
    switch (key) {
      case 3:  // ds5.haptics.hd.gain
      case 13:  // ds5.rumble.enabled
      case 14:  // ds5.rumble.strength
      case 15:  // ds5.rumble.curve
      case 16:  // ds5.rumble.lowGain
      case 17:  // ds5.rumble.highGain
      case 18:  // ds5.rumble.noiseGate
      case 24:  // ds5.trigger.adaptive.enabled
      case 25:  // ds5.trigger.adaptive.strength
      case 26:  // ds5.trigger.adaptive.leftStrength
      case 27:  // ds5.trigger.adaptive.rightStrength
      case 28:  // ds5.trigger.adaptive.feedbackScale
      case 29:  // ds5.trigger.adaptive.weaponScale
      case 30:  // ds5.trigger.adaptive.vibrationScale
      case 32:  // ds5.trigger.adaptive.offMode
      case 33:  // ds5.trigger.adaptive.rawPassthrough
      case 34:  // ds5.trigger.adaptive.modeMask
      case 1:  // ds5.profile.preset
      case 74:  // gamepad.touchpad.mode
      case 75:  // ds5.touchpad.output
      case 76:  // ds5.touchpad.sensitivity
      case 77:  // ds5.touchpad.scrollSpeed
      case 78:  // ds5.touchpad.reverseScroll
      case 79:  // ds5.touchpad.clickAction
      case 80:  // ds5.touchpad.twoFingerAction
      case 81:  // ds5.led.lightbar.mode
      case 82:  // ds5.led.lightbar.colorR
      case 83:  // ds5.led.lightbar.colorG
      case 84:  // ds5.led.lightbar.colorB
      case 85:  // ds5.led.lightbar.brightness
      case 86:  // ds5.led.player.mode
      case 87:  // ds5.led.player.index
      case 88:  // ds5.led.mute.mode
      case 89:  // ds5.led.releaseOnEnd
      case 90:  // ds5.audio.speakerLevel
      case 91:  // ds5.audio.headsetLevel
      case 92:  // ds5.audio.micLevel
      case 93:  // ds5.audio.speakerMute
      case 94:  // ds5.audio.micMute
      case 95:  // ds5.audio.route
      case 99:  // ds5.compat.preset
      case 35:  // ds5.stick.shape
      case 36:  // gamepad.stick.left.deadzoneInner
      case 37:  // gamepad.stick.left.deadzoneOuter
      case 38:  // gamepad.stick.left.antiDeadzone
      case 39:  // gamepad.stick.left.sensitivity
      case 40:  // gamepad.stick.left.curve
      case 41:  // gamepad.stick.left.invertX
      case 42:  // gamepad.stick.left.invertY
      case 43:  // gamepad.stick.right.deadzoneInner
      case 44:  // gamepad.stick.right.deadzoneOuter
      case 45:  // gamepad.stick.right.antiDeadzone
      case 46:  // gamepad.stick.right.sensitivity
      case 47:  // gamepad.stick.right.curve
      case 48:  // gamepad.stick.right.invertX
      case 49:  // gamepad.stick.right.invertY
      case 50:  // gamepad.trigger.left.deadzone
      case 51:  // gamepad.trigger.left.threshold
      case 52:  // gamepad.trigger.left.asButton
      case 53:  // gamepad.trigger.right.deadzone
      case 54:  // gamepad.trigger.right.threshold
      case 55:  // gamepad.trigger.right.asButton
      case 56:  // ds5.axis.trigger.left.rangeMin
      case 57:  // ds5.axis.trigger.left.rangeMax
      case 58:  // ds5.axis.trigger.left.antiDeadzone
      case 59:  // ds5.axis.trigger.left.curve
      case 60:  // ds5.axis.trigger.right.rangeMin
      case 61:  // ds5.axis.trigger.right.rangeMax
      case 62:  // ds5.axis.trigger.right.antiDeadzone
      case 63:  // ds5.axis.trigger.right.curve
      case 64:  // ds5.gyro.activation
      case 65:  // gamepad.gyro.sensitivity
      case 66:  // gamepad.gyro.deadzone
      case 67:  // ds5.gyro.filter
      case 68:  // ds5.gyro.filterHz
      case 69:  // ds5.gyro.filterBeta
      case 70:  // ds5.gyro.orientation
      case 71:  // ds5.gyro.yawOffsetDeg
      case 72:  // ds5.gyro.pitchOffsetDeg
      case 73:  // ds5.gyro.rollOffsetDeg
      case 106:  // ds5.identity.profile
        return true;
      default:
        return false;
    }
  }

  // 0 none, 1 legacyRumble, 2 legacyTrigger, 3 legacyAll.
  // Design 16.4.10: legacy rumble disables authored PCM and adaptive
  // triggers; legacy trigger disables the adaptive block only; legacy all
  // does both.  Release is mode 0x05, never 0x00 (16.4.3).
  inline bool
  allow_authored_pcm() {
    const auto preset = state().compat_preset.load(std::memory_order_acquire);
    return preset != 1u && preset != 3u;
  }

  inline void
  apply_compat_triggers(std::uint8_t &left_mode, std::uint8_t *left_params,
    std::uint8_t &right_mode, std::uint8_t *right_params) {
    if (state().compat_preset.load(std::memory_order_acquire) == 0u) return;
    const auto clear = [](std::uint8_t &mode, std::uint8_t *params) {
      mode = 0x05;
      if (params == nullptr) return;
      for (std::size_t index = 0; index < 10; ++index) params[index] = 0;
    };
    clear(left_mode, left_params);
    clear(right_mode, right_params);
  }

  inline void
  apply_pcm_gain(std::uint8_t *data, std::size_t bytes) {
    const auto gain = state().hd_gain.load(std::memory_order_acquire);
    if (gain == 1.0f || data == nullptr) return;
    auto *samples = reinterpret_cast<std::int16_t *>(data);
    for (std::size_t sample = 0; sample < bytes / sizeof(std::int16_t); ++sample) {
      const auto scaled = std::lround(static_cast<float>(samples[sample]) * gain);
      samples[sample] = static_cast<std::int16_t>(std::clamp(scaled, -32768L, 32767L));
    }
  }

  inline touchpad_control_t
  touchpad_control() {
    auto &value = state();
    touchpad_control_t control;
    control.output = static_cast<TouchpadOutput>(std::min(value.touchpad_output.load(std::memory_order_acquire), 3u));
    control.sensitivity = std::clamp(value.touchpad_sensitivity.load(std::memory_order_acquire), 0.1f, 5.0f);
    control.scroll_speed = std::clamp(value.touchpad_scroll_speed.load(std::memory_order_acquire), 0.1f, 5.0f);
    control.reverse_scroll = value.touchpad_reverse_scroll.load(std::memory_order_acquire);
    control.click = static_cast<TouchpadClickAction>(std::min(value.touchpad_click_action.load(std::memory_order_acquire), 3u));
    control.two_finger = static_cast<TouchpadTwoFingerAction>(std::min(value.touchpad_two_finger.load(std::memory_order_acquire), 2u));
    return control;
  }

  // 0 auto, 2 Xbox 360, 3 DualShock 4, 4 DualSense.  The Windows host uses
  // this only at the next virtual-device allocation, per design section 16.4.10.
  inline std::uint32_t
  identity_gamepad_mode() {
    return state().identity_mode.load(std::memory_order_acquire);
  }

  // Five player LEDs, centred like the console: --x--, -x-x-, x-x-x, xx-xx, xxxxx.
  // Values match ds5-linux-hid-playstation-20261031, dualsense_set_player_leds.
  inline std::uint32_t
  player_led_pattern(std::uint32_t index) {
    switch (index) {
      case 1:
        return 0x04u;
      case 2:
        return 0x0Au;
      case 3:
        return 0x15u;
      case 4:
        return 0x1Bu;
      case 5:
        return 0x1Fu;
      default:
        return 0u;
    }
  }

  // Follow game / fixed / off.  The game value is the last authored player LED
  // byte; the fixed value comes from key 87.
  inline std::uint32_t
  player_led_byte(std::uint32_t game_value) {
    auto &value = state();
    switch (value.player_led_mode.load(std::memory_order_acquire)) {
      case 1:
        return player_led_pattern(value.player_led_index.load(std::memory_order_acquire));
      case 2:
        return 0u;
      default:
        return game_value & 0x1Fu;
    }
  }

  // Follow game / on / off for output byte 9 (mute_button_led).
  inline std::uint32_t
  mute_led_byte(bool game_muted) {
    switch (state().mute_led_mode.load(std::memory_order_acquire)) {
      case 1:
        return 1u;
      case 2:
        return 0u;
      default:
        return game_muted ? 1u : 0u;
    }
  }

  // Output byte ranges from ds5-linux-hid-playstation-20261031,
  // dualsense_output_report_common.  Speaker follows the documented accepted
  // range [0x3d..0x64] so the low end stays reliable; headset and microphone
  // use their documented full ranges.
  inline std::uint8_t
  speaker_level_byte(float level) {
    return static_cast<std::uint8_t>(std::lround(0x3D + std::clamp(level, 0.0f, 1.0f) * (0x64 - 0x3D)));
  }

  inline std::uint8_t
  headset_level_byte(float level) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(level, 0.0f, 1.0f) * 0x7F));
  }

  inline std::uint8_t
  mic_level_byte(float level) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(level, 0.0f, 1.0f) * 0x40));
  }

  // 0 = headphone L/R, speaker muted; 3 = right channel to the speaker,
  // headphone muted.  The selector only carries one path at a time.
  inline std::uint8_t
  audio_output_path(std::uint32_t route, bool headset_detected) {
    if (route == 1) return 3u;
    if (route == 2) return 0u;
    return headset_detected ? 0u : 3u;
  }

  // audio_control2 preamp field: 0 = 0 dB, 2 = +6 dB.
  inline std::uint8_t
  audio_preamp_gain(std::uint32_t preamp) {
    return preamp == 1u ? 2u : 0u;
  }

  /**
   * @brief Fold the host tuning into the 0x5515 fields a game report produced (keys 86 to 88 and 90 to 95).
   *
   * Fixed or off player LEDs, a forced mute LED, a muted speaker or microphone, a forced audio path and a
   * level that differs from its default replace the game's value and mark the field valid, so they also
   * apply when the game did not send that field. A level at its default leaves the game in control.
   * Keys 96 (microphone route) and 97 (preamp) have no field in the extension and stay unconnected.
   */
  inline void
  apply_extension_tuning(::ds5::control_extension::output_t &fields, bool headset_detected) noexcept {
    namespace ext = ::ds5::control_extension;
    auto &value = state();
    if (value.player_led_mode.load(std::memory_order_acquire) != 0u) {
      fields.valid_mask |= ext::out_player_leds;
      fields.player_led_mask = static_cast<std::uint8_t>(player_led_byte(fields.player_led_mask));
    }
    if (value.mute_led_mode.load(std::memory_order_acquire) != 0u) {
      fields.valid_mask |= ext::out_mute_led;
      fields.mute_led = static_cast<std::uint8_t>(mute_led_byte(fields.mute_led != 0));
    }
    const auto speaker = value.speaker_level.load(std::memory_order_acquire);
    if (value.speaker_mute.load(std::memory_order_acquire)) {
      fields.valid_mask |= ext::out_speaker_volume;
      fields.speaker_volume = 0;
    }
    else if (speaker != 1.0f) {
      fields.valid_mask |= ext::out_speaker_volume;
      fields.speaker_volume = speaker_level_byte(speaker);
    }
    const auto headset = value.headset_level.load(std::memory_order_acquire);
    if (headset != 1.0f) {
      fields.valid_mask |= ext::out_headphone_volume;
      fields.headphone_volume = headset_level_byte(headset);
    }
    const auto microphone = value.mic_level.load(std::memory_order_acquire);
    if (microphone != 0.8f) {
      fields.valid_mask |= ext::out_mic_volume;
      fields.mic_volume = mic_level_byte(microphone);
    }
    if (value.mic_mute.load(std::memory_order_acquire)) {
      fields.valid_mask |= ext::out_mic_mute;
      fields.mic_mute = 1;
    }
    const auto route = value.audio_route.load(std::memory_order_acquire);
    if (route != 0u) {
      fields.valid_mask |= ext::out_audio_routing;
      fields.audio_routing = audio_output_path(route, headset_detected);
    }
  }

  inline void
  apply_lightbar(std::uint8_t &red, std::uint8_t &green, std::uint8_t &blue,
    std::uint8_t game_red, std::uint8_t game_green, std::uint8_t game_blue) {
    auto &value = state();
    const auto mode = value.lightbar_mode.load(std::memory_order_acquire);
    const auto brightness = std::clamp(value.lightbar_brightness.load(std::memory_order_acquire), 0.0f, 1.0f);
    const auto channel = [&](std::uint8_t game, std::uint32_t fixed) {
      const auto source = mode == 1 ? fixed : static_cast<std::uint32_t>(game);
      const auto scaled = std::lround(static_cast<float>(source) * brightness);
      return static_cast<std::uint8_t>(std::clamp(scaled, 0L, 255L));
    };
    red = mode == 2 ? std::uint8_t { 0 } : channel(game_red, value.lightbar_r.load(std::memory_order_acquire));
    green = mode == 2 ? std::uint8_t { 0 } : channel(game_green, value.lightbar_g.load(std::memory_order_acquire));
    blue = mode == 2 ? std::uint8_t { 0 } : channel(game_blue, value.lightbar_b.load(std::memory_order_acquire));
    value.lightbar_out_r.store(red, std::memory_order_release);
    value.lightbar_out_g.store(green, std::memory_order_release);
    value.lightbar_out_b.store(blue, std::memory_order_release);
  }

  /**
   * @brief The extension fields a pad should hold now, as a change against what the client was told.
   *
   * @p game_fields are the fields the game set so far (the newest value of each); the saved tuning
   * is folded into them every time, in a report and after a tuning save alike, so a value that goes
   * back to its default reaches the client at once and not with the game's next report. A player-LED
   * pattern the tuning forced and the game never set goes back to the number the pad shows from
   * connect (controller number + 1). The other fields the game never set have no default the wire can
   * name, so after a return to default they keep the last tuned value until the pad reconnects.
   */
  inline ::ds5::control_extension::output_t
  plan_extension_update(const ::ds5::control_extension::output_t &game_fields, ::ds5::control_extension::output_t &sent,
    std::uint32_t controller_number, bool headset_detected) noexcept {
    namespace ext = ::ds5::control_extension;
    auto folded = game_fields;
    apply_extension_tuning(folded, headset_detected);
    if ((folded.valid_mask & ext::out_player_leds) == 0 && (sent.valid_mask & ext::out_player_leds) != 0) {
      folded.valid_mask |= ext::out_player_leds;
      folded.player_led_mask = static_cast<std::uint8_t>(player_led_pattern(controller_number + 1));
    }
    return ext::take_changes(sent, folded);
  }

  /**
   * @brief What a pad shows after a tuning save, without waiting for the game's next report.
   *
   * Fixed colour and off apply at once, also before the game sent any colour. Follow-game
   * re-applies the last colour the game sent (the saved brightness still scales it) and does
   * nothing for a game that sent none. Nothing is emitted when the pad already shows that colour.
   */
  struct lightbar_push_t {
    bool emit { false };
    std::uint8_t red {};
    std::uint8_t green {};
    std::uint8_t blue {};
  };

  inline lightbar_push_t
  plan_lightbar_push(bool have_game_colour, const std::array<std::uint8_t, 3> &game,
    const std::array<std::uint8_t, 3> &shown, bool shown_before) {
    lightbar_push_t push;
    if (state().lightbar_mode.load(std::memory_order_acquire) == 0u && !have_game_colour) return push;
    apply_lightbar(push.red, push.green, push.blue, game[0], game[1], game[2]);
    push.emit = !shown_before || push.red != shown[0] || push.green != shown[1] || push.blue != shown[2];
    return push;
  }

  /** @brief Called after a profile was published to the runtime values (a tuning save or a load). */
  using published_handler_t = void (*)();

  inline std::atomic<published_handler_t> &
  published_handler() {
    static std::atomic<published_handler_t> value { nullptr };
    return value;
  }

  inline void
  notify_published() {
    if (const auto handler = published_handler().load(std::memory_order_acquire)) handler();
  }

  // One step toward black.  Quarter of the channel, and at least one count,
  // so a 0..255 colour reaches black in a few dozen reports with no timer
  // and no allocation.  releaseOnEnd false keeps the last colour.
  inline std::uint8_t
  fade_channel(std::uint8_t value) {
    if (value == 0) return 0;
    const auto drop = std::max<std::uint8_t>(1, static_cast<std::uint8_t>(value / 4));
    return static_cast<std::uint8_t>(value - drop);
  }

  inline bool
  session_release_step(std::uint8_t &red, std::uint8_t &green, std::uint8_t &blue) {
    auto &value = state();
    if (!value.release_on_end.load(std::memory_order_acquire)) return false;
    const auto current_r = value.lightbar_out_r.load(std::memory_order_acquire);
    const auto current_g = value.lightbar_out_g.load(std::memory_order_acquire);
    const auto current_b = value.lightbar_out_b.load(std::memory_order_acquire);
    if (current_r == 0 && current_g == 0 && current_b == 0) return false;
    red = fade_channel(current_r);
    green = fade_channel(current_g);
    blue = fade_channel(current_b);
    value.lightbar_out_r.store(red, std::memory_order_release);
    value.lightbar_out_g.store(green, std::memory_order_release);
    value.lightbar_out_b.store(blue, std::memory_order_release);
    return true;
  }

  template <typename Raise>
  inline void
  emit_session_release(Raise &&raise) {
    for (int step = 0; step < 64; ++step) {
      std::uint8_t red = 0;
      std::uint8_t green = 0;
      std::uint8_t blue = 0;
      if (!session_release_step(red, green, blue)) return;
      raise(red, green, blue);
    }
  }

  enum class motion_filter_state_e : std::uint32_t {
    uninitialized = 0,
    initialized = 1,
  };

  struct motion_filter_state_t {
    motion_filter_state_e state { motion_filter_state_e::uninitialized };
    std::uint32_t filter { 1 };
    float cutoff_hz { 30.0f };
    float beta { 0.05f };
    float value[3] { 0.0f, 0.0f, 0.0f };
    float derivative[3] { 0.0f, 0.0f, 0.0f };
  };

  inline float
  apply_curve(float value, std::uint32_t curve) noexcept {
    const auto magnitude = std::clamp(std::abs(value), 0.0f, 1.0f);
    const auto sign = value < 0.0f ? -1.0f : 1.0f;
    float shaped = magnitude;
    switch (curve) {
      case 1:  // enhanced
        shaped = magnitude * (2.0f - magnitude);
        break;
      case 2:  // quadratic
        shaped = magnitude * magnitude;
        break;
      case 3:  // cubic
        shaped = magnitude * magnitude * magnitude;
        break;
      case 4:  // easeOutQuad
        shaped = 1.0f - (1.0f - magnitude) * (1.0f - magnitude);
        break;
      case 5:  // easeOutCubic
        shaped = 1.0f - (1.0f - magnitude) * (1.0f - magnitude) * (1.0f - magnitude);
        break;
      default:
        break;
    }
    return sign * shaped;
  }

  inline float
  shape_axis(float value, float inner, float outer, float anti_deadzone,
    float sensitivity, std::uint32_t curve) noexcept {
    const auto sign = value < 0.0f ? -1.0f : 1.0f;
    const auto magnitude = std::clamp(std::abs(value), 0.0f, 1.0f);
    inner = std::clamp(inner, 0.0f, 0.999f);
    outer = std::clamp(outer, inner + 0.001f, 1.0f);
    anti_deadzone = std::clamp(anti_deadzone, 0.0f, 0.999f);
    auto normalized = magnitude <= inner ? 0.0f : (magnitude - inner) / (outer - inner);
    normalized = std::clamp(normalized, 0.0f, 1.0f);
    if (normalized != 0.0f && anti_deadzone != 0.0f) {
      normalized = anti_deadzone + normalized * (1.0f - anti_deadzone);
    }
    return std::clamp(sign * apply_curve(normalized, curve) * std::max(0.0f, sensitivity), -1.0f, 1.0f);
  }

  inline float
  decode_axis(std::int16_t value) noexcept {
    return value < 0 ? static_cast<float>(value) / 32768.0f : static_cast<float>(value) / 32767.0f;
  }

  inline std::int16_t
  encode_axis(float value) noexcept {
    value = std::clamp(value, -1.0f, 1.0f);
    const auto scaled = value < 0.0f ? value * 32768.0f : value * 32767.0f;
    return static_cast<std::int16_t>(std::lround(std::clamp(scaled, -32768.0f, 32767.0f)));
  }

  inline void
  apply_stick(std::int16_t &x, std::int16_t &y, bool left) noexcept {
    const auto &value = state();
    auto x_value = decode_axis(x);
    auto y_value = decode_axis(y);
    const auto inner = (left ? value.left_stick_deadzone_inner : value.right_stick_deadzone_inner).load(std::memory_order_acquire);
    const auto outer = (left ? value.left_stick_deadzone_outer : value.right_stick_deadzone_outer).load(std::memory_order_acquire);
    const auto anti = (left ? value.left_stick_anti_deadzone : value.right_stick_anti_deadzone).load(std::memory_order_acquire);
    const auto sensitivity = (left ? value.left_stick_sensitivity : value.right_stick_sensitivity).load(std::memory_order_acquire);
    const auto curve = (left ? value.left_stick_curve : value.right_stick_curve).load(std::memory_order_acquire);
    const auto invert_x = (left ? value.left_stick_invert_x : value.right_stick_invert_x).load(std::memory_order_acquire);
    const auto invert_y = (left ? value.left_stick_invert_y : value.right_stick_invert_y).load(std::memory_order_acquire);
    if (value.stick_shape.load(std::memory_order_acquire) == 1u) {
      const auto magnitude = std::min(1.0f, std::hypot(x_value, y_value));
      if (magnitude == 0.0f) {
        x_value = y_value = 0.0f;
      }
      else {
        const auto shaped = shape_axis(magnitude, inner, outer, anti, sensitivity, curve);
        const auto scale = shaped / magnitude;
        x_value *= scale;
        y_value *= scale;
      }
    }
    else {
      x_value = shape_axis(x_value, inner, outer, anti, sensitivity, curve);
      y_value = shape_axis(y_value, inner, outer, anti, sensitivity, curve);
    }
    if (invert_x) x_value = -x_value;
    if (invert_y) y_value = -y_value;
    x = encode_axis(x_value);
    y = encode_axis(y_value);
  }

  /** Whether the raw trigger value reaches the button threshold (keys 51 and 54). */
  inline bool
  trigger_pressed(std::uint8_t raw, bool left) noexcept {
    auto &value = state();
    const auto threshold = std::clamp((left ? value.left_trigger_threshold : value.right_trigger_threshold).load(std::memory_order_acquire), 0.05f, 1.0f);
    return static_cast<float>(raw) / 255.0f >= threshold;
  }

  /** With asButton (keys 52 and 55) the analog axis is 0 or 255 by the threshold decision. */
  inline void
  apply_trigger_as_button(std::uint8_t &analog, bool pressed, bool left) noexcept {
    if (!(left ? state().left_trigger_as_button : state().right_trigger_as_button).load(std::memory_order_acquire)) return;
    analog = pressed ? 255u : 0u;
  }

  inline std::uint8_t
  encode_trigger(float value) noexcept {
    return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
  }

  inline void
  apply_trigger(std::uint8_t &raw, bool left) noexcept {
    const auto &value = state();
    const auto normalized = static_cast<float>(raw) / 255.0f;
    const auto deadzone = (left ? value.left_trigger_deadzone : value.right_trigger_deadzone).load(std::memory_order_acquire);
    const auto range_min = (left ? value.left_trigger_range_min : value.right_trigger_range_min).load(std::memory_order_acquire);
    const auto range_max = (left ? value.left_trigger_range_max : value.right_trigger_range_max).load(std::memory_order_acquire);
    const auto anti = (left ? value.left_trigger_anti_deadzone : value.right_trigger_anti_deadzone).load(std::memory_order_acquire);
    const auto curve = (left ? value.left_trigger_curve : value.right_trigger_curve).load(std::memory_order_acquire);
    raw = encode_trigger(shape_axis(normalized, std::max(deadzone, range_min), range_max, anti, 1.0f, curve));
  }

  /** @brief The trigger bytes and digital L2/R2 bits the report builder uses (keys 50 to 63). */
  struct shaped_triggers_t {
    std::uint8_t left {};
    std::uint8_t right {};
    bool l2_pressed {};
    bool r2_pressed {};
  };

  /**
   * @brief Judge the raw trigger bytes against the button thresholds, shape the axes, then let
   *        asButton binarise them.  The digital bits judge the raw value; shaping does not move them.
   */
  inline shaped_triggers_t
  shape_triggers(std::uint8_t left_raw, std::uint8_t right_raw) noexcept {
    shaped_triggers_t result { left_raw, right_raw, trigger_pressed(left_raw, true), trigger_pressed(right_raw, false) };
    apply_trigger(result.left, true);
    apply_trigger(result.right, false);
    apply_trigger_as_button(result.left, result.l2_pressed, true);
    apply_trigger_as_button(result.right, result.r2_pressed, false);
    return result;
  }

  inline void
  apply_orientation(float &x, float &y, float &z, std::uint32_t orientation,
    float yaw, float pitch, float roll) noexcept {
    if (orientation == 1u) {
      const auto screen_x = -y;
      const auto screen_y = x;
      x = screen_x;
      y = screen_y;
    }
    else if (orientation == 2u) {
      const auto world_x = x;
      const auto world_y = z;
      const auto world_z = -y;
      x = world_x;
      y = world_y;
      z = world_z;
    }
    if (orientation != 3u) return;
    constexpr float kPi = 3.14159265358979323846f;
    const auto yaw_radians = yaw * kPi / 180.0f;
    const auto pitch_radians = pitch * kPi / 180.0f;
    const auto roll_radians = roll * kPi / 180.0f;
    const auto cy = std::cos(yaw_radians), sy = std::sin(yaw_radians);
    const auto cp = std::cos(pitch_radians), sp = std::sin(pitch_radians);
    const auto cr = std::cos(roll_radians), sr = std::sin(roll_radians);
    const auto rx = cy * cp * x + (cy * sp * sr - sy * cr) * y + (cy * sp * cr + sy * sr) * z;
    const auto ry = sy * cp * x + (sy * sp * sr + cy * cr) * y + (sy * sp * cr - cy * sr) * z;
    const auto rz = -sp * x + cp * sr * y + cp * cr * z;
    x = rx;
    y = ry;
    z = rz;
  }

  // Source: ds5-linux-hid-playstation-20261031 and ds5-gamepad-parameters-
  // 20261031, applied in the order specified by design §16.4.4–§16.4.6.
  inline bool
  apply_gyro(float &x, float &y, float &z, motion_filter_state_t &filter_state, float dt_seconds = 1.0f / 250.0f) noexcept {
    const auto &value = state();
    if (value.gyro_activation.load(std::memory_order_acquire) == 0u) {
      filter_state.state = motion_filter_state_e::uninitialized;
      x = y = z = 0.0f;
      return false;
    }
    const auto sensitivity = value.gyro_sensitivity.load(std::memory_order_acquire);
    const auto deadzone = std::clamp(value.gyro_deadzone.load(std::memory_order_acquire), 0.0f, 1.0f);
    const auto filter = value.gyro_filter.load(std::memory_order_acquire);
    const auto cutoff = std::clamp(value.gyro_filter_hz.load(std::memory_order_acquire), 1.0f, 240.0f);
    const auto beta = std::clamp(value.gyro_filter_beta.load(std::memory_order_acquire), 0.0f, 1.0f);
    apply_orientation(x, y, z, value.gyro_orientation.load(std::memory_order_acquire),
      value.gyro_yaw_offset_deg.load(std::memory_order_acquire),
      value.gyro_pitch_offset_deg.load(std::memory_order_acquire),
      value.gyro_roll_offset_deg.load(std::memory_order_acquire));
    x *= sensitivity;
    y *= sensitivity;
    z *= sensitivity;
    /* The deadzone is a fraction of the 2048-count half range of the pad's gyroscope, 16.4 counts per
     * degree per second, so 0.05 is 6 degrees per second. The rate arrives in degrees per second.
     * Below the threshold nothing moves; above it only the excess does, so there is no jump at the edge. */
    const auto magnitude = std::sqrt(x * x + y * y + z * z);
    const auto threshold_dps = deadzone * 2048.0f / 16.4f;
    if (magnitude <= threshold_dps) {
      x = y = z = 0.0f;
    }
    else {
      const auto scale = (magnitude - threshold_dps) / magnitude;
      x *= scale;
      y *= scale;
      z *= scale;
    }
    if (filter_state.state == motion_filter_state_e::uninitialized || filter_state.filter != filter ||
        filter_state.cutoff_hz != cutoff || filter_state.beta != beta) {
      filter_state.state = motion_filter_state_e::initialized;
      filter_state.filter = filter;
      filter_state.cutoff_hz = cutoff;
      filter_state.beta = beta;
      filter_state.value[0] = x;
      filter_state.value[1] = y;
      filter_state.value[2] = z;
      filter_state.derivative[0] = filter_state.derivative[1] = filter_state.derivative[2] = 0.0f;
    }
    else if (filter == 1u || filter == 2u) {
      /* The smoothing follows the time between the samples, not a fixed 250 Hz: over Bluetooth the
       * samples come 128 times a second, and a per-sample factor made for 250 Hz halved the cutoff. */
      const auto dt = std::clamp(dt_seconds, 0.001f, 0.05f);
      const auto alpha = std::clamp(1.0f - std::exp(-2.0f * 3.14159265358979323846f * cutoff * dt), 0.0f, 1.0f);
      for (std::size_t index = 0; index < 3; ++index) {
        const auto derivative = (index == 0 ? x : index == 1 ? y :
                                                               z) -
                                filter_state.value[index];
        const auto derivative_alpha = filter == 2u ? std::clamp(alpha + beta * std::abs(derivative), 0.0f, 1.0f) : alpha;
        filter_state.derivative[index] = derivative;
        filter_state.value[index] += derivative_alpha * derivative;
      }
    }
    else {
      /* No filter: the output is the sample itself. The state used to keep the first sample forever. */
      filter_state.value[0] = x;
      filter_state.value[1] = y;
      filter_state.value[2] = z;
    }
    x = filter_state.value[0];
    y = filter_state.value[1];
    z = filter_state.value[2];
    return true;
  }

  struct haptics_tuning_t {
    bool enabled { true };
    float low_gain { 1.0f };
    float high_gain { 1.0f };
    float crossover_hz { 160.0f };
    float noise_gate { 0.002f };
    float intensity { 0.8f };
    float sharpness { 0.5f };
    std::uint32_t latency_ms { 10 };
    std::uint32_t smoothing_ms { 2 };
  };

  inline haptics_tuning_t
  haptics_tuning() {
    const auto &value = state();
    haptics_tuning_t result;
    result.enabled = value.haptics_mode.load(std::memory_order_acquire) != 3u;
    const auto db_to_linear = [](float db) {
      return std::pow(10.0f, db / 20.0f);
    };
    result.low_gain = std::clamp(db_to_linear(value.hd_low_band_db.load(std::memory_order_acquire)), 0.0f, 8.0f);
    result.high_gain = std::clamp(db_to_linear(value.hd_high_band_db.load(std::memory_order_acquire)), 0.0f, 8.0f);
    result.crossover_hz = std::clamp(value.hd_crossover_hz.load(std::memory_order_acquire), 40.0f, 400.0f);
    result.noise_gate = std::clamp(value.hd_noise_gate.load(std::memory_order_acquire), 0.0f, 0.060f);
    result.intensity = std::clamp(value.ir_intensity.load(std::memory_order_acquire), 0.0f, 1.0f);
    result.sharpness = std::clamp(value.ir_sharpness.load(std::memory_order_acquire), 0.0f, 1.0f);
    result.latency_ms = std::clamp(value.hd_latency_ms.load(std::memory_order_acquire), 0u, 50u);
    result.smoothing_ms = std::clamp(value.hd_smoothing_ms.load(std::memory_order_acquire), 0u, 20u);
    return result;
  }

  /**
   * @brief Whether the native PCM source emits authored haptics for the endpoint.
   *
   * "auto", "authoredPcm" and "ir" need a bound four-channel endpoint; "off"
   * never emits.  The client still chooses raw PCM, IR v2 or its local
   * synthesizer from its own capabilities, so the host does not invent a
   * second routing decision here.
   */
  inline bool
  pcm_source_enabled(bool endpoint_active) {
    const auto mode = state().haptics_mode.load(std::memory_order_acquire);
    if (mode == 3u) return false;
    return endpoint_active;
  }

  struct rumble_tuning_t {
    std::uint32_t hold_ms { 60 };
    std::uint32_t send_interval_ms { 20 };
    std::uint32_t watchdog_ms { 100 };
    std::uint32_t max_duration_ms { 5000 };
    std::uint32_t max_rate_hz { 250 };
    std::uint32_t effective_interval_ms { 20 };
  };

  inline rumble_tuning_t
  rumble_tuning() {
    const auto &value = state();
    rumble_tuning_t result;
    result.hold_ms = std::clamp(value.rumble_hold_ms.load(std::memory_order_acquire), 0u, 200u);
    result.send_interval_ms = std::clamp(value.rumble_send_interval_ms.load(std::memory_order_acquire), 4u, 50u);
    result.watchdog_ms = std::clamp(value.rumble_watchdog_ms.load(std::memory_order_acquire), 20u, 1000u);
    result.max_duration_ms = std::clamp(value.rumble_max_duration_ms.load(std::memory_order_acquire), 0u, 10000u);
    result.max_rate_hz = std::clamp(value.rumble_max_rate_hz.load(std::memory_order_acquire), 10u, 1000u);
    const auto rate_interval = (1000u + result.max_rate_hz - 1u) / result.max_rate_hz;
    result.effective_interval_ms = std::max(result.send_interval_ms, rate_interval);
    return result;
  }
}  // namespace ds5::tuning::runtime

// The rumble shaping block lives in rumble_shape.h so this header stops
// growing; included here so every existing consumer sees it unchanged.
#include "src/ds5/rumble_shape.h"
