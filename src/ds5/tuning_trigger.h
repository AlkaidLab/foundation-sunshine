/**
 * @file src/ds5/tuning_trigger.h
 * @brief Lock-free per-mode rewriting of DualSense adaptive-trigger blocks.
 *
 * The 11-byte block is the vendor-specific payload the game writes for one
 * trigger.  Start/end positions and mode bytes are preserved; only the
 * strength/amplitude fields of modes whose layout is cross-checked are
 * rewritten.  Unknown modes stay byte-for-byte identical when the combined
 * strength is neutral, and are left alone when they cannot be scaled safely.
 *
 * Sources: docs/architecture/ds5-platform-design.zh.md section 16.4.3 on
 * branch docs/ds5-tuning-profile (commit 29f7673fb, read 2026-10-31);
 * ds5-apple-gctrigger-20261031 for the five public mode classes and their
 * parameter intent; ds5-linux-hid-playstation-20261031 for the output-report
 * valid bits; ds5-pydualsense-20261031, ds5-onimusha-trigger-20261031 and
 * ds5-horizonhaptics-trigger-20261031 for the independently observed
 * 0x01/0x21/0x26 byte meanings and zone packing; Nielk1 TriggerEffectGenerator
 * (MIT, read 2026-11-01) for the 0x25 and 0x06 layouts.
 */
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "tuning_runtime.h"

namespace ds5::tuning::trigger {
  constexpr std::size_t block_size = 11;
  using block_t = std::array<std::uint8_t, block_size>;

  enum class Class : std::uint32_t {
    feedback = 0,
    weapon = 1,
    vibration = 2,
    slope = 3,
    raw = 4,
  };

  enum class OffMode : std::uint32_t {
    release = 0,
    clear = 1,
    hold = 2,
  };

  enum class Action : std::uint8_t {
    none,
    passthrough,
    replace,
    replace_then_zero,
  };

  struct config_t {
    bool enabled { true };
    float strength { 1.0f };
    float left_strength { 1.0f };
    float right_strength { 1.0f };
    float feedback_scale { 1.0f };
    float weapon_scale { 1.0f };
    float vibration_scale { 1.0f };
    float slope_scale { 1.0f };
    std::uint32_t mode_mask { 31u };
    bool raw_passthrough { false };
    OffMode off_mode { OffMode::release };
  };

  inline config_t
  read_config() {
    const auto &value = runtime::state();
    config_t result;
    result.enabled = value.trigger_enabled.load(std::memory_order_acquire);
    result.strength = value.trigger_strength.load(std::memory_order_acquire);
    result.left_strength = value.trigger_left_strength.load(std::memory_order_acquire);
    result.right_strength = value.trigger_right_strength.load(std::memory_order_acquire);
    result.feedback_scale = value.trigger_feedback_scale.load(std::memory_order_acquire);
    result.weapon_scale = value.trigger_weapon_scale.load(std::memory_order_acquire);
    result.vibration_scale = value.trigger_vibration_scale.load(std::memory_order_acquire);
    result.slope_scale = value.trigger_slope_scale.load(std::memory_order_acquire);
    result.mode_mask = value.trigger_mode_mask.load(std::memory_order_acquire);
    result.raw_passthrough = value.trigger_raw_passthrough.load(std::memory_order_acquire);
    switch (value.trigger_off_mode.load(std::memory_order_acquire)) {
      case 1:
        result.off_mode = OffMode::clear;
        break;
      case 2:
        result.off_mode = OffMode::hold;
        break;
      default:
        result.off_mode = OffMode::release;
        break;
    }
    return result;
  }

  inline block_t
  off_block() {
    block_t result {};
    result[0] = 0x05;
    return result;
  }

  inline block_t
  zero_block() {
    return {};
  }

  inline bool
  is_off(const block_t &block) {
    return block[0] == 0x00u || block[0] == 0x05u;
  }

  inline Class
  classify(std::uint8_t mode) {
    switch (mode) {
      case 0x01:
      case 0x21:
        return Class::feedback;
      case 0x02:
      case 0x22:
      case 0x25:
        return Class::weapon;
      case 0x03:
      case 0x06:
      case 0x26:
        return Class::vibration;
      case 0x04:
        return Class::slope;
      default:
        return Class::raw;
    }
  }

  inline float
  mode_scale(Class trigger_class, const config_t &config) {
    switch (trigger_class) {
      case Class::feedback:
        return config.feedback_scale;
      case Class::weapon:
        return config.weapon_scale;
      case Class::vibration:
        return config.vibration_scale;
      case Class::slope:
        return config.slope_scale;
      case Class::raw:
        return 1.0f;
    }
    return 1.0f;
  }

  inline float
  effective_scale(Class trigger_class, bool left, const config_t &config) {
    const auto side = left ? config.left_strength : config.right_strength;
    return config.strength * side * mode_scale(trigger_class, config);
  }

  inline std::uint8_t
  scale_byte(std::uint8_t value, float scale) {
    const auto scaled = std::lround(static_cast<float>(value) * scale);
    return static_cast<std::uint8_t>(std::clamp(scaled, 0L, 255L));
  }

  /**
   * @brief Rewrites the ten-zone mask/packed-strength payload of modes 0x21/0x26.
   *
   * The mask is two little-endian bytes at 1..2; zones 0..9 use three bits at
   * 3..6, where a stored value of 0..7 means strength 1..8.  Frequency, when
   * present, stays at byte 9.  Bits outside the ten zones are preserved.
   */
  inline bool
  scale_zones(block_t &block, float scale) {
    const auto mask = static_cast<std::uint16_t>(block[1] | (static_cast<std::uint16_t>(block[2]) << 8));
    const auto packed = static_cast<std::uint32_t>(block[3]) | (static_cast<std::uint32_t>(block[4]) << 8) |
                        (static_cast<std::uint32_t>(block[5]) << 16) | (static_cast<std::uint32_t>(block[6]) << 24);
    std::uint16_t next_mask = static_cast<std::uint16_t>(mask & 0xfc00u);
    std::uint32_t next_packed = packed & 0xc0000000u;
    for (std::uint32_t zone = 0; zone < 10u; ++zone) {
      if ((mask & (1u << zone)) == 0u) continue;
      const auto level = ((packed >> (3u * zone)) & 0x07u) + 1u;
      const auto scaled = std::clamp<std::int32_t>(
        std::lround(static_cast<float>(level) * scale), 0, 8);
      if (scaled == 0) continue;
      next_mask = static_cast<std::uint16_t>(next_mask | (1u << zone));
      next_packed |= static_cast<std::uint32_t>(scaled - 1u) << (3u * zone);
    }
    const bool changed = next_mask != mask || next_packed != packed;
    block[1] = static_cast<std::uint8_t>(next_mask);
    block[2] = static_cast<std::uint8_t>(next_mask >> 8);
    block[3] = static_cast<std::uint8_t>(next_packed);
    block[4] = static_cast<std::uint8_t>(next_packed >> 8);
    block[5] = static_cast<std::uint8_t>(next_packed >> 16);
    block[6] = static_cast<std::uint8_t>(next_packed >> 24);
    return changed;
  }

  inline bool
  blocks_equal(const block_t &left, const block_t &right) {
    return std::equal(left.begin(), left.end(), right.begin());
  }

  /**
   * @brief Rewrites one trigger block for the current profile.
   *
   * @param input The game-authored block.
   * @param left True for the left trigger.
   * @param config A snapshot read before the output-report loop.
   * @param output The block to send; valid for replace actions.
   * @param action none to send nothing, replace for one block, replace_then_zero
   *        for the clear off-mode (send the off block and then a zero block).
   * @return True when the caller should send @p output.
   */
  inline bool
  rewrite(const block_t &input, bool left, const config_t &config, block_t &output, Action &action) {
    const block_t source = input;
    output = source;
    action = Action::passthrough;
    if (is_off(source)) {
      return true;
    }

    const auto trigger_class = classify(source[0]);
    const auto class_bit = 1u << static_cast<std::uint32_t>(trigger_class);
    if (!config.enabled || (config.mode_mask & class_bit) == 0u) {
      switch (config.off_mode) {
        case OffMode::hold:
          action = Action::none;
          return false;
        case OffMode::clear:
          output = off_block();
          action = Action::replace_then_zero;
          return true;
        case OffMode::release:
          output = off_block();
          action = Action::replace;
          return true;
      }
      return false;
    }

    const auto scale = effective_scale(trigger_class, left, config);
    if (std::abs(scale - 1.0f) <= 1e-4f) {
      action = Action::passthrough;
      return true;
    }

    if (trigger_class == Class::raw) {
      if (!config.raw_passthrough) {
        output = off_block();
        action = Action::replace;
        return true;
      }
      action = Action::passthrough;
      return true;
    }

    bool changed = false;
    switch (input[0]) {
      case 0x01:
        output[2] = scale_byte(source[2], scale);
        changed = output[2] != source[2];
        break;
      case 0x02:
        output[3] = scale_byte(source[3], scale);
        changed = output[3] != source[3];
        break;
      case 0x06:
        output[2] = scale_byte(source[2], scale);
        changed = output[2] != source[2];
        break;
      case 0x21:
      case 0x26:
        changed = scale_zones(output, scale);
        break;
      case 0x25: {
        // Byte 3 stores strength 1..8 as 0..7; the start and stop zones stay at 1..2.
        const auto level = static_cast<std::uint32_t>(source[3] & 0x07u) + 1u;
        const auto scaled = std::clamp<std::int32_t>(std::lround(static_cast<float>(level) * scale), 0, 8);
        if (scaled == 0) {
          output = off_block();
          action = Action::replace;
          return true;
        }
        output[3] = static_cast<std::uint8_t>(scaled - 1);
        changed = output[3] != source[3];
        break;
      }
      default:
        // The family is known but the vendor byte layout is not cross-checked;
        // preserve it rather than writing a guessed strength byte.
        action = Action::passthrough;
        return true;
    }

    if (!changed) {
      output = source;
      action = Action::passthrough;
      return true;
    }
    if (output[0] == 0x01u && output[2] == 0u) {
      output = off_block();
    }
    else if (output[0] == 0x02u && output[3] == 0u) {
      output = off_block();
    }
    else if (output[0] == 0x06u && output[2] == 0u) {
      output = off_block();
    }
    else if ((output[0] == 0x21u || output[0] == 0x26u) && (output[1] | output[2]) == 0u) {
      output = off_block();
    }
    action = Action::replace;
    return true;
  }

  /**
   * @brief Captures a game block into the cached state and returns what to send.
   *
   * A repeated block is not re-sent.  The caches and output are fixed-size
   * arrays owned by the caller; no allocation or lock is taken.
   */
  inline bool
  capture(const block_t &input, bool left, const config_t &config,
    block_t &state, block_t &output, Action &action) {
    if (blocks_equal(input, state)) {
      action = Action::none;
      return false;
    }
    state = input;
    return rewrite(input, left, config, output, action);
  }

  inline bool
  capture(const std::uint8_t *source, bool left, const config_t &config,
    block_t &state, block_t &output, Action &action) {
    block_t input {};
    std::copy_n(source, block_size, input.begin());
    return capture(input, left, config, state, output, action);
  }

  struct plan_t {
    block_t left {};
    block_t right {};
    std::uint8_t flags { 0 };
    bool clear { false };
    bool clear_left { false };
    bool clear_right { false };
  };

  /**
   * @brief Per-controller trigger state for live policy and strength changes.
   *
   * The tracker keeps the last game-authored block for each side and the last
   * block actually sent.  It is called on every output report, so disabling,
   * masking or re-scaling a mode takes effect even when the game is holding a
   * steady effect and does not set a new valid-trigger bit.
   */
  class tracker_t {
  public:
    plan_t
    plan(const std::uint8_t *payload, std::uint8_t valid_flag0, const config_t &config) {
      plan_t result;
      if ((valid_flag0 & 0x08u) != 0u) {
        std::copy_n(payload + 22, block_size, left_state_.begin());
        left_valid_ = true;
      }
      if ((valid_flag0 & 0x04u) != 0u) {
        std::copy_n(payload + 11, block_size, right_state_.begin());
        right_valid_ = true;
      }

      Action left_action {};
      Action right_action {};
      block_t desired_left {};
      block_t desired_right {};
      rewrite(left_state_, true, config, desired_left, left_action);
      rewrite(right_state_, false, config, desired_right, right_action);
      const auto choose = [](bool valid, Action action, const block_t &desired, const block_t &sent) {
        return valid && action != Action::none ? desired : sent;
      };
      const auto left_out = choose(left_valid_, left_action, desired_left, sent_left_);
      const auto right_out = choose(right_valid_, right_action, desired_right, sent_right_);
      const bool left_changed = left_valid_ && left_action != Action::none &&
                                (!sent_valid_ || !blocks_equal(left_out, sent_left_));
      const bool right_changed = right_valid_ && right_action != Action::none &&
                                 (!sent_valid_ || !blocks_equal(right_out, sent_right_));
      if (!left_changed && !right_changed) {
        return result;
      }

      result.left = left_out;
      result.right = right_out;
      result.flags = static_cast<std::uint8_t>((left_changed ? 0x08u : 0u) |
                                               (right_changed ? 0x04u : 0u));
      result.clear = left_action == Action::replace_then_zero ||
                     right_action == Action::replace_then_zero;
      result.clear_left = left_action == Action::replace_then_zero;
      result.clear_right = right_action == Action::replace_then_zero;
      sent_left_ = left_out;
      sent_right_ = right_out;
      sent_valid_ = true;
      return result;
    }

  private:
    block_t left_state_ {};
    block_t right_state_ {};
    block_t sent_left_ {};
    block_t sent_right_ {};
    bool left_valid_ { false };
    bool right_valid_ { false };
    bool sent_valid_ { false };
  };
}  // namespace ds5::tuning::trigger
