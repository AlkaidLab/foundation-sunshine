/**
 * @file src/ds5/rumble_shape.h
 * @brief The rumble switch, per-motor gains and shaping every rumble path shares.
 *
 * Included at the end of tuning_runtime.h, after state() exists, so existing
 * consumers keep a single include.  The native report path (apply_rumble) and
 * the legacy audio-derived ERM reduction both read these keys through
 * rumble_shape(), so the switch and the channel gains cannot drift into
 * meaning different things on the two paths.  Key ids follow ALK-DS5T/1:
 * 13 ds5.rumble.enabled, 14 strength, 15 curve, 16 lowGain, 17 highGain,
 * 18 noiseGate.
 */
#pragma once

#include "src/ds5/tuning_runtime.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace ds5::tuning::runtime {
  /** @brief One acquire-consistent read of the rumble keys (ids 13 to 18). */
  struct rumble_shape_t {
    bool enabled { true };
    float low_gain { 1.0f };
    float high_gain { 1.0f };
    float strength { 1.0f };
    float curve { 1.0f };
    float noise_gate { 0.020f };
  };

  inline rumble_shape_t
  rumble_shape() {
    const auto &value = state();
    rumble_shape_t shape;
    shape.enabled = value.rumble_enabled.load(std::memory_order_acquire);
    shape.low_gain = value.rumble_low_gain.load(std::memory_order_acquire);
    shape.high_gain = value.rumble_high_gain.load(std::memory_order_acquire);
    shape.strength = value.rumble_strength.load(std::memory_order_acquire);
    shape.curve = value.rumble_curve.load(std::memory_order_acquire);
    shape.noise_gate = value.rumble_noise_gate.load(std::memory_order_acquire);
    return shape;
  }

  inline std::uint16_t
  shape_rumble(std::uint16_t value, float gain, float strength, float curve, float gate) {
    const auto normalized = static_cast<float>(value) / 65535.0f;
    if (normalized <= gate) return 0;
    const auto shaped = static_cast<float>(std::pow(normalized, curve)) * gain * strength;
    return static_cast<std::uint16_t>(std::clamp(shaped, 0.0f, 1.0f) * 65535.0f + 0.5f);
  }

  inline void
  apply_rumble(std::uint16_t &left, std::uint16_t &right) {
    const auto shape = rumble_shape();
    if (!shape.enabled) {
      left = 0;
      right = 0;
      return;
    }
    left = shape_rumble(left, shape.low_gain, shape.strength, shape.curve, shape.noise_gate);
    right = shape_rumble(right, shape.high_gain, shape.strength, shape.curve, shape.noise_gate);
  }
}  // namespace ds5::tuning::runtime
