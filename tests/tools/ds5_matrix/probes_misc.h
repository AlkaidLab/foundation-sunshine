/**
 * @file tests/tools/ds5_matrix/probes_misc.h
 * @brief Touchpad, preset, compatibility and identity probes.
 */
#pragma once

#include "src/ds5/touchpad_mapper.h"
#include "tests/tools/ds5_matrix/probes_haptics.h"

namespace ds5::matrix {
  /**
   * @brief One finger drag, a two-finger sweep and a click, resolved by the published touchpad
   *        control exactly as the controller touch and button handlers of the input layer do.
   *
   * Controller output copies the contact into the input report; host-pointer output goes through the
   * pointer mapper (the same header the input layer calls).  The precision touchpad and the pointer
   * injection themselves live in the platform layer and are not driven here.
   */
  inline sig_t
  touch_probe(rig_t &rig) {
    using namespace tuning::runtime;
    auto control = touchpad_control();
    touchpad_pointer_t mapper;
    double report_x = -1, report_flags = -1;
    if (control.output == TouchpadOutput::controller) {
      const auto report = rig.touch(0, 0, 0.5f, 0.5f);
      report_x = report[34];
      report_flags = report[33];
      rig.touch(2, 0, 0.5f, 0.5f);
    }
    double move_x = 0, move_y = 0, scroll = 0, right_press = 0, right_release = 0, click_button = 0, click_consume = 0, click_release = 0;
    if (control.output == TouchpadOutput::host_pointer) {
      const auto take = [&](const touchpad_pointer_event_t &event) {
        move_x += event.move_x;
        move_y += event.move_y;
        scroll += event.scroll;
        if (event.mouse_button == 3 && !event.mouse_release) right_press += 1;
        if (event.mouse_button == 3 && event.mouse_release) right_release += 1;
      };
      take(mapper.touch(TouchPhase::down, 0, 0.5f, 0.5f, control));
      take(mapper.touch(TouchPhase::move, 0, 0.6f, 0.5f, control));
      take(mapper.touch(TouchPhase::move, 0, 0.6f, 0.7f, control));
      take(mapper.touch(TouchPhase::up, 0, 0.6f, 0.7f, control));
      take(mapper.touch(TouchPhase::down, 0, 0.4f, 0.4f, control));
      take(mapper.touch(TouchPhase::down, 1, 0.6f, 0.4f, control));
      take(mapper.touch(TouchPhase::move, 0, 0.4f, 0.3f, control));
      take(mapper.touch(TouchPhase::move, 1, 0.6f, 0.3f, control));
      take(mapper.touch(TouchPhase::up, 0, 0.4f, 0.3f, control));
      take(mapper.touch(TouchPhase::up, 1, 0.6f, 0.3f, control));
      const auto press = mapper.button(true, control);
      click_button = press.mouse_button;
      click_consume = press.consume ? 1 : 0;
      const auto release = mapper.button(false, control);
      click_release = release.mouse_release ? release.mouse_button : 0;
    }
    return { double(static_cast<std::uint32_t>(control.output)), report_x, report_flags, move_x, move_y, scroll, right_press, right_release,
      click_button, click_consume, click_release };
  }

  /** @brief Rumble, game lightbar, PCM peak and an adaptive trigger: everything a preset bundle touches. */
  inline sig_t
  preset_probe(rig_t &rig) {
    const auto rumble = rumble_probe(rig);
    const auto light = lightbar_probe(rig);
    const auto pcm = pcm_probe(rig);
    const auto trigger = trigger_probe({ 0x21 })(rig);
    return { rumble[0], rumble[1], light[0], light[1], light[2], pcm[0], pcm[1], trigger[1] };
  }

  /** @brief Adaptive trigger type and PCM chunk count: the two things the legacy presets switch off. */
  inline sig_t
  compat_probe(rig_t &rig) {
    const auto trigger = trigger_probe({ 0x21 })(rig);
    const auto pcm = pcm_probe(rig);
    return { trigger[1], trigger[0], pcm[0] };
  }

  inline sig_t
  identity_probe(rig_t &rig) {
    return { static_cast<double>(rig.identity_mode()) };
  }
}  // namespace ds5::matrix
