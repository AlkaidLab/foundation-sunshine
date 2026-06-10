/**
 * @file src/input_activity.cpp
 * @brief Definitions for input activity detection used by the VRR input activity boost.
 */
// standard includes
#include <cmath>
#include <cstdint>

// lib includes
#include <moonlight-common-c/src/Input.h>
#include <moonlight-common-c/src/Limelight.h>

// local includes
#include "config.h"
#include "input_activity.h"
#include "utility.h"

namespace input::activity {

  // Ignore analog jitter/drift unless the input crosses a small deadzone or changes meaningfully
  // while active. This matches the common "edge + deadzone + hysteresis" pattern used by input systems.
  constexpr std::int16_t STICK_DEADZONE = 2048;
  constexpr std::int16_t STICK_DELTA = 1024;
  constexpr std::uint8_t TRIGGER_THRESHOLD = 8;
  constexpr std::uint8_t TRIGGER_DELTA = 4;

  namespace {

    bool
    stick_outside_deadzone(std::int16_t value) {
      return std::abs(static_cast<int>(value)) >= STICK_DEADZONE;
    }

    bool
    trigger_has_meaningful_change(std::uint8_t previous, std::uint8_t current) {
      const auto previous_active = previous >= TRIGGER_THRESHOLD;
      const auto current_active = current >= TRIGGER_THRESHOLD;
      if (previous_active != current_active) {
        return true;
      }

      return (previous_active || current_active) &&
             std::abs(static_cast<int>(current) - static_cast<int>(previous)) >= TRIGGER_DELTA;
    }

    bool
    stick_has_meaningful_change(std::int16_t previous, std::int16_t current) {
      const auto previous_active = stick_outside_deadzone(previous);
      const auto current_active = stick_outside_deadzone(current);
      if (previous_active != current_active) {
        return true;
      }

      return (previous_active || current_active) &&
             std::abs(static_cast<int>(current) - static_cast<int>(previous)) >= STICK_DELTA;
    }

  }  // namespace

  bool
  tracker_t::evaluate_controller(_NV_MULTI_CONTROLLER_PACKET *packet) {
    const auto controller_number = util::endian::little(packet->controllerNumber);
    if (controller_number < 0 || controller_number >= static_cast<int>(controllers_.size())) {
      return false;
    }

    const auto button_flags =
      static_cast<std::uint32_t>(util::endian::little(static_cast<std::uint16_t>(packet->buttonFlags))) |
      (static_cast<std::uint32_t>(util::endian::little(static_cast<std::uint16_t>(packet->buttonFlags2))) << 16);
    const auto left_trigger = packet->leftTrigger;
    const auto right_trigger = packet->rightTrigger;
    const auto left_stick_x = util::endian::little(packet->leftStickX);
    const auto left_stick_y = util::endian::little(packet->leftStickY);
    const auto right_stick_x = util::endian::little(packet->rightStickX);
    const auto right_stick_y = util::endian::little(packet->rightStickY);

    auto &state = controllers_[controller_number];
    bool should_trigger = false;

    if (!state.initialized) {
      should_trigger =
        button_flags != 0 ||
        left_trigger >= TRIGGER_THRESHOLD ||
        right_trigger >= TRIGGER_THRESHOLD ||
        stick_outside_deadzone(left_stick_x) ||
        stick_outside_deadzone(left_stick_y) ||
        stick_outside_deadzone(right_stick_x) ||
        stick_outside_deadzone(right_stick_y);
    }
    else {
      should_trigger =
        button_flags != state.button_flags ||
        trigger_has_meaningful_change(state.left_trigger, left_trigger) ||
        trigger_has_meaningful_change(state.right_trigger, right_trigger) ||
        stick_has_meaningful_change(state.left_stick_x, left_stick_x) ||
        stick_has_meaningful_change(state.left_stick_y, left_stick_y) ||
        stick_has_meaningful_change(state.right_stick_x, right_stick_x) ||
        stick_has_meaningful_change(state.right_stick_y, right_stick_y);
    }

    state.initialized = true;
    state.button_flags = button_flags;
    state.left_trigger = left_trigger;
    state.right_trigger = right_trigger;
    state.left_stick_x = left_stick_x;
    state.left_stick_y = left_stick_y;
    state.right_stick_x = right_stick_x;
    state.right_stick_y = right_stick_y;

    return should_trigger;
  }

  bool
  tracker_t::evaluate(_NV_INPUT_HEADER *payload) {
    switch (util::endian::little(payload->magic)) {
      case MOUSE_MOVE_REL_MAGIC_GEN5:
        return config::input.mouse &&
               (util::endian::big(reinterpret_cast<PNV_REL_MOUSE_MOVE_PACKET>(payload)->deltaX) != 0 ||
                util::endian::big(reinterpret_cast<PNV_REL_MOUSE_MOVE_PACKET>(payload)->deltaY) != 0);
      case MOUSE_MOVE_ABS_MAGIC:
      case MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5:
      case MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5:
        return config::input.mouse;
      case KEY_DOWN_EVENT_MAGIC:
      case KEY_UP_EVENT_MAGIC:
      case UTF8_TEXT_EVENT_MAGIC:
        return config::input.keyboard;
      case SS_TOUCH_MAGIC:
      case SS_PEN_MAGIC:
        return config::input.mouse;
      case SS_CONTROLLER_TOUCH_MAGIC:
        return config::input.controller;
      case SCROLL_MAGIC_GEN5:
        return config::input.mouse &&
               (util::endian::big(reinterpret_cast<PNV_SCROLL_PACKET>(payload)->scrollAmt1) != 0 ||
                util::endian::big(reinterpret_cast<PNV_SCROLL_PACKET>(payload)->scrollAmt2) != 0);
      case SS_HSCROLL_MAGIC:
        return config::input.mouse && util::endian::big(reinterpret_cast<PSS_HSCROLL_PACKET>(payload)->scrollAmount) != 0;
      case MULTI_CONTROLLER_MAGIC_GEN5:
        return config::input.controller && evaluate_controller(reinterpret_cast<PNV_MULTI_CONTROLLER_PACKET>(payload));
      default:
        return false;
    }
  }

  void
  tracker_t::reset() {
    for (auto &state : controllers_) {
      state = {};
    }
  }

}  // namespace input::activity
