/**
 * @file tests/tools/ds5_matrix/rumble_styles.h
 * @brief Output-report styles real engines use for rumble, and which of them produce a rumble message.
 *
 * Sources (public, read 2026-11-01): SDL src/joystick/hidapi/SDL_hidapi_ps5.c, HIDAPI_DriverPS5_UpdateEffects
 * (enhanced rumble: enable bits 3 = 0x04 with enable bits 1 = 0x02; legacy: enable bits 1 = 0x01 | 0x02 with
 * the motors halved; stopping leaves the rumble bits off); Unity Input System DualShockGamepadHID.cs,
 * DualSenseGamepadHID.SetMotorSpeedsAndLightBarColor (enable flags 1 = 0x01 | 0x02, flags 2 = 0x04 with the
 * colour at payload offsets 44 to 46); Linux hid-playstation DS_OUTPUT_VALID_FLAG0/2 (compatible vibration
 * 0x01, haptics select 0x02, improved rumble emulation 0x04 in flag 2).
 */
#pragma once

#include <iostream>

#include "tests/tools/ds5_matrix/probes_io.h"

namespace ds5::matrix {
  struct rumble_style_t {
    const char *name;
    const char *origin;
    std::uint8_t flag0;
    std::uint8_t flag1;
    std::uint8_t flag2;
    std::uint8_t left;
    std::uint8_t right;
    char intent;  ///< 'v' the game asks for vibration, 's' it asks to stop, 'n' the report carries no motor request
  };

  inline std::vector<rumble_style_t>
  rumble_styles() {
    return {
      { "compat vibration only", "hid-playstation, DS_OUTPUT_VALID_FLAG0_COMPATIBLE_VIBRATION", 0x01, 0x00, 0x00, 100, 200, 'v' },
      { "haptics select only, with motor bytes", "flag 0x02 alone: the host moves the motors with the bytes it carries", 0x02, 0x00, 0x00, 100, 200, 'v' },
      { "compat vibration + haptics select", "Unity SetMotorSpeedsAndLightBarColor, SDL legacy rumble", 0x03, 0x00, 0x00, 100, 200, 'v' },
      { "all flag0 bits", "full-report writers", 0xff, 0x00, 0x00, 100, 200, 'v' },
      { "full 0xff / 0xf7 / 0x07", "full-report writers (all enables)", 0xff, 0xf7, 0x07, 100, 200, 'v' },
      { "improved emulation only (flag2 0x04)", "hid-playstation, DS_OUTPUT_VALID_FLAG2_IMPROVED_RUMBLE_EMULATION", 0x00, 0x00, 0x04, 100, 200, 'v' },
      { "haptics select + improved emulation", "SDL enhanced rumble (firmware 2.24 and newer)", 0x02, 0x00, 0x04, 100, 200, 'v' },
      { "compat + select + improved emulation", "combined writers", 0x03, 0x00, 0x04, 100, 200, 'v' },
      { "Unity rumble + lightbar", "Unity: flags1 0x01|0x02, flags2 0x04 (LED colour)", 0x03, 0x04, 0x00, 100, 200, 'v' },
      { "SDL legacy, motors halved", "SDL legacy: motors >> 1", 0x03, 0x00, 0x00, 50, 100, 'v' },
      { "SDL stop (enable bits cleared)", "SDL: rumble 0 leaves the rumble bits off", 0x00, 0x00, 0x00, 0, 0, 's' },
      { "SDL stop with haptics select", "SDL RumbleStart effect: bit 0x02 only", 0x02, 0x00, 0x00, 0, 0, 's' },
      { "compat vibration stop", "flag 0x01 with zero motors", 0x01, 0x00, 0x00, 0, 0, 's' },
      { "improved emulation stop", "flag2 0x04 with zero motors", 0x02, 0x00, 0x04, 0, 0, 's' },
    };
  }

  /** @brief Writes every style as a game would and prints one STYLE row each. */
  inline void
  run_rumble_styles(rig_t &rig) {
    rig.clear_store();
    for (const auto &style : rumble_styles()) {
      rig.output(out_report(0x00));  // idle between styles, so the haptics-state change detector starts alike
      if (style.intent == 's') {
        // A stop only means something while a rumble runs.
        auto running = out_report(0x01);
        running[3] = 200;
        running[4] = 100;
        rig.output(running);
      }
      auto report = out_report(style.flag0, style.flag1, style.flag2);
      report[3] = style.right;
      report[4] = style.left;
      if (style.flag1 & 0x04) {
        report[45] = 10;
        report[46] = 20;
        report[47] = 30;
      }
      const auto messages = rig.output(report);
      const auto *rumble = last_of(messages, fb_kind::rumble);
      const bool state = last_of(messages, fb_kind::state) != nullptr;
      const char *verdict = rumble ? "rumble message" : "no rumble message";
      std::string note;
      if (style.intent == 'v' && !rumble) note = "NOT delivered: the game asks for vibration";
      if (style.intent == 's' && !rumble) note = "no stop message: the client keeps its last value until its own timeout";
      if (style.intent == 's' && rumble) note = "stop delivered as zero motors";
      if (style.intent == 'n' && !rumble) note = "correct: no motor request in this report";
      if (style.intent == 'n' && rumble) note = "rumble sent although the report carries no motor request";
      std::cout << "STYLE\t" << style.name << "\t" << style.origin << "\t0x" << std::hex << int(style.flag0) << "/0x" << int(style.flag1) << "/0x"
                << int(style.flag2) << std::dec << "\tL=" << int(style.left) << " R=" << int(style.right) << "\t" << verdict << "\t"
                << (rumble ? "low=" + std::to_string(rumble->low) + " high=" + std::to_string(rumble->high) : std::string("-")) << "\t"
                << (state ? "haptics-state sent" : "no haptics-state") << "\t" << note << "\t" << rig.name() << std::endl;
    }
  }
}  // namespace ds5::matrix
