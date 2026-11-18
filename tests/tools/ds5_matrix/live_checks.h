/**
 * @file tests/tools/ds5_matrix/live_checks.h
 * @brief Session-active cases: a save made while a pad is attached and streaming has to reach the pad
 *        by itself. The pad is attached once; each case saves a value and then reads what the host raised
 *        without any game report in between. A change that only shows at the game's next report, or at the
 *        next attach, is a FAIL here.
 */
#pragma once

#include <iostream>

#include "tests/tools/ds5_matrix/probes_io.h"

namespace ds5::matrix {
  struct live_row_t {
    std::string name;
    bool pass {};
    std::string evidence;
  };

  inline bool
  has_kind(const std::vector<fb_t> &messages, fb_kind kind) {
    for (const auto &message : messages) {
      if (message.kind == kind) return true;
    }
    return false;
  }

  inline std::vector<live_row_t>
  run_live_checks(rig_t &rig) {
    std::vector<live_row_t> rows;
    const int attaches_before = rig.attach_count();
    std::string why;
    const auto rewind = [&] { post_ok(rig, default_values(), &why); };
    rig.clear_store();

    {  // adaptive triggers: the effect the game holds is shaped again by the saved strength
      live_row_t row { "adaptive trigger strength, game holds a steady effect" };
      auto flush = out_report(0x0c);
      rig.output(flush);
      auto report = out_report(0x0c);
      const auto block = trigger_block(0x02);
      std::copy(block.begin(), block.end(), report.begin() + 11);
      std::copy(block.begin(), block.end(), report.begin() + 22);
      rig.output(report);
      const auto before = rig.trigger_view();
      const bool saved = post_ok(rig, { { "ds5.trigger.adaptive.strength", 0.5 } }, &why);
      const auto after = rig.trigger_view();
      row.pass = saved && has_kind(rig.pushed(), fb_kind::trigger) && (before.left != after.left || before.right != after.right);
      row.evidence = "saved=" + std::string(saved ? "yes" : "no") + " trigger messages raised by the save: " + std::to_string(std::count_if(rig.pushed().begin(), rig.pushed().end(), [](const fb_t &m) { return m.kind == fb_kind::trigger; })) +
                     " parameter before " + std::to_string(before.left[1]) + " after " + std::to_string(after.left[1]) + " (no game report in between)";
      rows.push_back(row);
      rewind();
    }

    {  // rumble: a rumble the game holds is shaped again
      live_row_t row { "rumble strength, game holds a steady rumble" };
      auto report = out_report(0x01);
      report[3] = 100;
      report[4] = 200;
      const auto first = last_of(rig.output(report), fb_kind::rumble);
      const int low_before = first ? first->low : -1;
      const bool saved = post_ok(rig, { { "ds5.rumble.strength", 0.5 } }, &why);
      const auto *pushed = last_of(rig.pushed(), fb_kind::rumble);
      row.pass = saved && pushed != nullptr && pushed->low != low_before;
      row.evidence = "rumble low motor before " + std::to_string(low_before) + " after the save " + (pushed ? std::to_string(pushed->low) : std::string("no message")) + " (no game report in between)";
      rows.push_back(row);
      auto stop = out_report(0x01);
      rig.output(stop);
      rewind();
    }

    {  // lightbar: a fixed colour reaches the pad at once
      live_row_t row { "lightbar fixed colour" };
      auto report = out_report(0, 0x04);
      report[45] = 200;
      report[46] = 100;
      report[47] = 50;
      rig.output(report);
      const bool saved = post_ok(rig, { { "ds5.led.lightbar.mode", "fixed" }, { "ds5.led.lightbar.colorR", 52 }, { "ds5.led.lightbar.colorG", 199 }, { "ds5.led.lightbar.colorB", 89 } }, &why);
      const auto *rgb = last_of(rig.pushed(), fb_kind::rgb);
      row.pass = saved && rgb != nullptr && rgb->r == 52 && rgb->g == 199 && rgb->b == 89;
      row.evidence = rgb ? "colour raised by the save: " + std::to_string(rgb->r) + "," + std::to_string(rgb->g) + "," + std::to_string(rgb->b) : "no colour raised by the save";
      rows.push_back(row);
      rewind();
      const auto *back = last_of(rig.pushed(), fb_kind::rgb);
      live_row_t restored { "lightbar back to the game colour after the default is restored" };
      restored.pass = back != nullptr && back->r == 200 && back->g == 100 && back->b == 50;
      restored.evidence = back ? "colour raised by the restore: " + std::to_string(back->r) + "," + std::to_string(back->g) + "," + std::to_string(back->b) : "no colour raised by the restore";
      rows.push_back(restored);
    }

    {  // player LEDs
      live_row_t row { "player LED pattern fixed" };
      const bool saved = post_ok(rig, { { "ds5.led.player.mode", "fixed" }, { "ds5.led.player.index", 3 } }, &why);
      bool seen = false;
      for (const auto &m : rig.pushed()) seen = seen || (m.kind == fb_kind::ext && (m.mask & 0x01u) != 0u);
      row.pass = saved && seen;
      row.evidence = std::string("extension message with the player LEDs raised by the save: ") + (seen ? "yes" : "no");
      rows.push_back(row);
      rewind();
    }

    {
      live_row_t row { "the pad stayed attached for the whole run" };
      row.pass = rig.attach_count() == attaches_before;
      row.evidence = "attach count " + std::to_string(attaches_before) + " at the start, " + std::to_string(rig.attach_count()) + " at the end";
      rows.push_back(row);
    }
    return rows;
  }
}  // namespace ds5::matrix
