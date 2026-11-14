/**
 * @file tests/tools/ds5_matrix/cases_device.h
 * @brief Cases for adaptive triggers 24 to 34, lightbar and LEDs 81 to 89, audio 90 to 95, compatibility 99 and identity 106.
 */
#pragma once

#include "tests/tools/ds5_matrix/cases_common.h"

namespace ds5::matrix {
  namespace cases_detail {
    constexpr std::size_t trigger_stride = 15;  ///< count, left type, right type, six left and six right parameters

    inline std::size_t
    trig_at(std::size_t position, std::size_t field) { return position * trigger_stride + field; }

    /** @brief The strength byte a mode carries, in the parameter slot the rewriter scales, for a scale of @p s. */
    inline double
    trig_expected(std::uint8_t mode, double s) {
      const auto level = [&](int levels) { return std::max(0L, std::lround(levels * s)); };
      switch (mode) {
        case 0x01:
        case 0x02:
        case 0x06:
          return std::clamp(std::lround(100 * s), 0L, 255L);
        case 0x25:
          return static_cast<double>(level(4) - 1);
        case 0x21:
        case 0x26: {
          const auto n = static_cast<std::uint32_t>(level(4) - 1);
          return static_cast<double>((n | (n << 3) | ((n & 3u) << 6)) & 0xffu);
        }
        default:
          return 0;
      }
    }

    inline std::size_t
    trig_param_slot(std::uint8_t mode) { return (mode == 0x01 || mode == 0x06) ? 1 : 2; }

    /** @brief Every side of every mode carries the strength the scale function asks for. */
    inline std::string
    trig_check(const sig_t &sig, const std::vector<std::uint8_t> &modes, const std::function<double(std::uint8_t, bool)> &scale) {
      std::string why;
      for (std::size_t p = 0; p < modes.size(); ++p) {
        for (const bool left : { true, false }) {
          const auto s = scale(modes[p], left);
          const auto got = sig[trig_at(p, (left ? 3 : 9) + trig_param_slot(modes[p]))];
          const auto wanted = std::fabs(s - 1.0) < 1e-6 ? (modes[p] == 0x21 || modes[p] == 0x26 ? 0xDB : (modes[p] == 0x25 ? 3 : 100)) : trig_expected(modes[p], s);
          if (std::fabs(got - wanted) > 0.5) why += "mode " + fmt(modes[p]) + (left ? " left " : " right ") + "strength " + fmt(got) + " (want " + fmt(wanted) + "); ";
        }
      }
      return why;
    }

    /** @brief After restoring the default and one game heartbeat, the client holds what the game itself sent. */
    inline std::function<std::string(rig_t &, std::string &)>
    ext_restore_extra(int key, json tuned, json setup, json restore_setup = json()) {
      if (restore_setup.is_null()) restore_setup = setup;
      return [key, tuned, setup, restore_setup](rig_t &rig, std::string &detail) {
        const auto *descriptor = tuning::find_descriptor(static_cast<std::uint32_t>(key));
        rig.post(merged(setup, tuned));
        ext_probe(rig);
        rig.output(ext_report(0x44, 0x44, 0x44, 3, 1, true, 5, 0x1f));
        rig.output(ext_report(0x34, 0x62, 0x24, 1, 0, false, 4, 0x02));
        rig.post(merged(restore_setup, { { descriptor->key, to_json(tuning::default_value(*descriptor)) } }));
        rig.output(out_report(0x01));
        const auto view = ext_view_sig(rig.ext_view());
        const sig_t game { 0x02, 0, 0x62, 0x34, 0x24, 0, 1, 4 };
        detail += " | after restore and a heartbeat the client holds " + fmt(view);
        if (!approx(view, game, 0)) return std::string("after restoring the default the client keeps the tuned value until the game re-sends the field (holds ") + fmt(view) + ", game sent " + fmt(game) + "); ";
        return std::string {};
      };
    }
  }  // namespace cases_detail

  inline void
  append_device_cases(std::vector<case_t> &table) {
    using namespace cases_detail;
    builder_t builder { table };
    const auto simple = [&](int key, std::function<sig_t(rig_t &)> probe, std::vector<value_case_t> values, auto expect, json setup = json::object()) {
      builder.simple(key, std::move(probe), std::move(values), expect, std::move(setup));
    };
    const std::vector<std::uint8_t> modes { 0x01, 0x21, 0x02, 0x25, 0x06, 0x26 };
    const auto all_modes = trigger_probe(modes);

    simple(24, all_modes, { val(kv(24, false), "off") }, [modes](const sig_t &, const vecs &v) {
      std::string why;
      for (std::size_t p = 0; p < modes.size(); ++p) {
        if (v[0][trig_at(p, 1)] != 5 || v[0][trig_at(p, 2)] != 5) why += "mode " + fmt(modes[p]) + " not released; ";
      }
      return why;
    });
    const auto uniform = [&](int key, double left_scale, double right_scale, std::set<std::uint8_t> affected, std::vector<value_case_t> values) {
      simple(key, all_modes, std::move(values), [=](const sig_t &, const vecs &v) {
        return trig_check(v[0], modes, [=](std::uint8_t mode, bool left) { return affected.contains(mode) ? (left ? left_scale : right_scale) : 1.0; });
      });
    };
    const std::set<std::uint8_t> everything { 0x01, 0x21, 0x02, 0x25, 0x06, 0x26 };
    uniform(25, 0.5, 0.5, everything, { val(kv(25, 0.5), "x0.5"), val(kv(25, 2.0), "x2") });
    uniform(26, 0.5, 1.0, everything, { val(kv(26, 0.5), "left x0.5") });
    uniform(27, 1.0, 0.5, everything, { val(kv(27, 0.5), "right x0.5") });
    uniform(28, 0.5, 0.5, { 0x01, 0x21 }, { val(kv(28, 0.5), "feedback x0.5") });
    uniform(29, 0.5, 0.5, { 0x02, 0x25 }, { val(kv(29, 0.5), "weapon x0.5") });
    uniform(30, 0.5, 0.5, { 0x06, 0x26 }, { val(kv(30, 0.5), "vibration x0.5") });
    simple(32, trigger_probe({ 0x01 }), { val(kv(32, "clear"), "clear"), val(kv(32, "hold"), "hold") }, [](const sig_t &b, const vecs &v) {
      std::string why;
      if (b[0] != 1 || b[1] != 5) why += "release baseline is not one 0x05 message; ";
      if (v[0][0] != 2 || v[0][1] != 0) why += "clear should send the release block then a zero block; ";
      if (v[1][0] != 0) why += "hold should send nothing; ";
      return why; }, kv(24, false));
    simple(33, trigger_probe({ 0x10 }), { val(kv(33, true), "raw passes") }, [](const sig_t &b, const vecs &v) {
      std::string why;
      if (b[1] != 5) why += "a scaled unknown mode should be released when raw passthrough is off; ";
      if (v[0][1] != 0x10 || v[0][3] != 0x11) why += "raw block not forwarded unchanged; ";
      return why; }, kv(25, 0.5));
    {
      const std::vector<std::uint8_t> mask_modes { 0x01, 0x02, 0x06, 0x04, 0x10 };
      simple(34, trigger_probe(mask_modes), { val(kv(34, 29), "no weapon"), val(kv(34, 0), "none") }, [mask_modes](const sig_t &b, const vecs &v) {
        std::string why;
        for (std::size_t p = 0; p < mask_modes.size(); ++p) {
          const bool weapon = mask_modes[p] == 0x02;
          if (weapon && v[0][trig_at(p, 1)] != 5) why += "weapon mode still sent with the weapon bit cleared; ";
          if (!weapon && v[0][trig_at(p, 1)] != b[trig_at(p, 1)]) why += "mode " + fmt(mask_modes[p]) + " changed with only the weapon bit cleared; ";
          if (v[1][trig_at(p, 1)] != 5) why += "mode " + fmt(mask_modes[p]) + " still sent with an empty mask; ";
        }
        return why;
      });
    }

    // ---- lightbar and LEDs ----
    simple(81, lightbar_probe, { val(kv(81, "fixed"), "fixed"), val(kv(81, "off"), "off") }, [](const sig_t &, const vecs &v) {
      std::string why;
      for (std::size_t i = 0; i < 6; ++i) {
        if (v[0][i] != (i % 3 == 0 ? 30 : i % 3 == 1 ? 60 :
                                                       90)) why += "fixed colour not 30/60/90 at " + std::to_string(i) + "; ";
        if (v[1][i] != 0) why += "off not black; ";
      }
      return why;
    },
      { { key_name(82), 30 }, { key_name(83), 60 }, { key_name(84), 90 } });
    for (const int channel : { 0, 1, 2 }) {
      simple(82 + channel, lightbar_probe, { val(kv(82 + channel, 200), "200") }, [channel](const sig_t &b, const vecs &v) {
        std::string why;
        if (v[0][channel] != 200 || v[0][3 + channel] != 200) why += "channel did not become 200; ";
        for (std::size_t i = 0; i < 6; ++i) if (i % 3 != static_cast<std::size_t>(channel) && v[0][i] != b[i]) why += "another channel moved; ";
        return why; }, kv(81, "fixed"));
    }
    simple(85, lightbar_probe, { val(kv(85, 0.5), "x0.5"), val(kv(85, 0.0), "0") }, [](const sig_t &, const vecs &v) {
      std::string why;
      if (v[0][0] != 100 || v[0][1] != 50 || v[0][2] != 25 || v[0][3] != 128) why += "0.5 did not halve the colour: " + fmt(v[0]);
      for (const auto x : v[1])
        if (x != 0) why += "brightness 0 is not black; ";
      return why;
    });
    simple(89, release_probe, { val(kv(89, false), "keep") }, [](const sig_t &b, const vecs &v) {
      std::string why;
      if (b[0] < 2 || b[1] != 0 || b[2] != 0 || b[3] != 0) why += "default release did not fade to black: " + fmt(b);
      if (v[0][0] != 0) why += "releaseOnEnd=false still sent fade messages; ";
      return why;
    });

    // ---- extension: player, mute, audio (86-88, 90-95). Index 0..7 round one, 8..15 round two. ----
    const auto ext_case = [&](int key, std::vector<value_case_t> values, std::set<std::size_t> moved, json setup, json restore_with,
                            std::function<std::string(const sig_t &, const vecs &)> expect) {
      case_t c;
      c.key = key;
      c.probe = ext_probe;
      c.values = std::move(values);
      c.setup = setup;
      c.expect = [moved, expect](const sig_t &b, const vecs &v) {
        std::string why = expect(b, v);
        for (const auto &sig : v) why += only(b, sig, [&](std::size_t i) { return moved.contains(i); }, "an unrelated field");
        return why;
      };
      c.extra = ext_restore_extra(key, restore_with, setup, key == 87 ? kv(86, "followGame") : json());
      builder.add(std::move(c));
    };
    ext_case(86, { val(kv(86, "fixed"), "fixed"), val(kv(86, "off"), "off") }, { 0, 8 }, kv(87, 3), kv(86, "fixed"), [](const sig_t &, const vecs &v) {
      std::string why;
      if (v[0][0] != 0x15 || v[0][8] != 0x15) why += "fixed pattern for index 3 is not 0x15; ";
      if (v[1][0] != 0 || v[1][8] != 0) why += "off is not 0; ";
      return why;
    });
    ext_case(87, { val(kv(87, 1), "1"), val(kv(87, 3), "3"), val(kv(87, 5), "5") }, { 0, 8 }, kv(86, "fixed"), kv(87, 3), [](const sig_t &b, const vecs &v) {
      std::string why;
      if (b[0] != 0) why += "fixed with index auto is expected to be dark but is " + fmt(b[0]) + "; ";
      if (v[0][0] != 0x04 || v[1][0] != 0x15 || v[2][0] != 0x1f) why += "pattern bytes are not 0x04 0x15 0x1f; ";
      return why;
    });
    ext_case(88, { val(kv(88, "on"), "on"), val(kv(88, "off"), "off") }, { 1, 9 }, json::object(), kv(88, "on"), [](const sig_t &, const vecs &v) {
      std::string why;
      if (v[0][1] != 1 || v[0][9] != 1) why += "on is not 1; ";
      if (v[1][1] != 0 || v[1][9] != 0) why += "off is not 0; ";
      return why;
    });
    ext_case(90, { val(kv(90, 0.5), "0.5"), val(kv(90, 0.0), "0") }, { 2, 10 }, json::object(), kv(90, 0.5), [](const sig_t &, const vecs &v) {
      std::string why;
      if (v[0][2] != 81 || v[0][10] != 81) why += "level 0.5 is not byte 81; ";
      if (v[1][2] != 61) why += "level 0 is not the documented floor 61; ";
      return why;
    });
    ext_case(91, { val(kv(91, 0.5), "0.5") }, { 3, 11 }, json::object(), kv(91, 0.5), [](const sig_t &, const vecs &v) { return v[0][3] == 64 && v[0][11] == 64 ? std::string {} : std::string("level 0.5 is not byte 64; "); });
    ext_case(92, { val(kv(92, 0.5), "0.5") }, { 4, 12 }, json::object(), kv(92, 0.5), [](const sig_t &, const vecs &v) { return v[0][4] == 32 && v[0][12] == 32 ? std::string {} : std::string("level 0.5 is not byte 32; "); });
    ext_case(93, { val(kv(93, true), "muted") }, { 2, 10 }, json::object(), kv(93, true), [](const sig_t &, const vecs &v) { return v[0][2] == 0 && v[0][10] == 0 ? std::string {} : std::string("speaker volume is not 0; "); });
    ext_case(94, { val(kv(94, true), "muted") }, { 5, 13 }, json::object(), kv(94, true), [](const sig_t &, const vecs &v) { return v[0][5] == 1 && v[0][13] == 1 ? std::string {} : std::string("microphone mute is not 1; "); });
    ext_case(95, { val(kv(95, "speaker"), "speaker"), val(kv(95, "headset"), "headset") }, { 6, 14 }, json::object(), kv(95, "speaker"), [](const sig_t &, const vecs &v) {
      std::string why;
      if (v[0][6] != 3 || v[0][14] != 3) why += "speaker route is not 3; ";
      if (v[1][6] != 0 || v[1][14] != 0) why += "headset route is not 0; ";
      return why;
    });

    // ---- compatibility and identity ----
    simple(99, compat_probe, { val(kv(99, "legacyRumble"), "legacyRumble"), val(kv(99, "legacyTrigger"), "legacyTrigger"), val(kv(99, "legacyAll"), "legacyAll") }, [](const sig_t &b, const vecs &v) {
      std::string why;
      if (b[0] != 0x21 || b[2] == 0) why += "baseline is not a 0x21 trigger plus PCM; ";
      for (const auto &sig : v)
        if (sig[0] != 5) why += "legacy preset left the trigger block in place; ";
      if (v[0][2] != 0 || v[2][2] != 0) why += "legacyRumble/legacyAll still pass PCM; ";
      if (v[1][2] == 0) why += "legacyTrigger dropped PCM; ";
      return why;
    });
    simple(106, identity_probe, { val(kv(106, "xbox360"), "xbox360"), val(kv(106, "ds4"), "ds4"), val(kv(106, "ds5"), "ds5") }, [](const sig_t &b, const vecs &v) {
      return (b[0] == 0 && v[0][0] == 2 && v[1][0] == 3 && v[2][0] == 4) ? std::string {} : std::string("identity modes are not 0 / 2 / 3 / 4; ");
    });
  }
}  // namespace ds5::matrix
