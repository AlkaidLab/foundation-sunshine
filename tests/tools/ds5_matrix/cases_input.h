/**
 * @file tests/tools/ds5_matrix/cases_input.h
 * @brief Cases for stick, trigger-axis, gyro and touchpad keys (35 to 80) and the unavailable keys.
 */
#pragma once

#include "tests/tools/ds5_matrix/cases_common.h"

namespace ds5::matrix {
  namespace cases_detail {
    constexpr double stick_tol = 0.025;

    /** @brief Index of component @p c (0 lx, 1 ly, 2 rx, 3 ry) in state @p s of the stick probe. */
    inline std::size_t
    st(std::size_t s, std::size_t c) { return s * 4 + c; }

    inline std::string
    expect_at(const sig_t &sig, std::size_t index, double wanted, double tol, const char *what) {
      return near(sig[index], wanted, tol) ? std::string {} : std::string(what) + " " + fmt(sig[index]) + " (want " + fmt(wanted) + "); ";
    }

    /** @brief Cases for one stick (cx, cy are its component slots in the probe). */
    inline void
    stick_cases(builder_t &builder, std::size_t cx, std::size_t cy, int first_key) {
      const auto side = [=](std::size_t i) { return i % 4 == cx || i % 4 == cy; };
      const auto key = [&](int offset) { return first_key + offset; };
      const auto mk = [&](int offset, std::vector<value_case_t> values, std::function<std::string(const sig_t &, const vecs &)> body, json setup = json::object()) {
        builder.simple(key(offset), stick_probe, std::move(values), [=](const sig_t &b, const vecs &v) {
          std::string why = body(b, v);
          for (const auto &sig : v) why += only(b, sig, side, "the other stick");
          return why; }, std::move(setup));
      };
      mk(0, { val(kv(key(0), 0.3), "0.3") }, [=](const sig_t &, const vecs &v) {
        return expect_at(v[0], st(0, cx), 0.286, stick_tol, "inner 0.3 at 0.5") + expect_at(v[0], st(0, cy), 0.0, stick_tol, "inner 0.3 at 0.25") + expect_at(v[0], st(1, cx), 0.0, stick_tol, "inner 0.3 at 0.06");
      });
      mk(1, { val(kv(key(1), 0.6), "0.6") }, [=](const sig_t &, const vecs &v) {
        return expect_at(v[0], st(0, cx), 0.833, stick_tol, "outer 0.6 at 0.5") + expect_at(v[0], st(0, cy), -0.417, stick_tol, "outer 0.6 at -0.25");
      });
      mk(2, { val(kv(key(2), 0.3), "0.3") }, [=](const sig_t &, const vecs &v) {
        return expect_at(v[0], st(1, cx), 0.343, stick_tol, "anti 0.3 at 0.06") + expect_at(v[0], st(1, cy), 0.0, stick_tol, "anti 0.3 at 0");
      });
      mk(3, { val(kv(key(3), 1.5), "1.5") }, [=](const sig_t &, const vecs &v) {
        return expect_at(v[0], st(0, cx), 0.75, stick_tol, "sensitivity 1.5 at 0.5") + expect_at(v[0], st(0, cy), -0.375, stick_tol, "sensitivity 1.5 at -0.25");
      });
      mk(4, { val(kv(key(4), "enhanced"), "enhanced"), val(kv(key(4), "quadratic"), "quadratic"), val(kv(key(4), "cubic"), "cubic"), val(kv(key(4), "easeOutQuad"), "easeOutQuad"), val(kv(key(4), "easeOutCubic"), "easeOutCubic") },
        [=](const sig_t &, const vecs &v) {
          return expect_at(v[0], st(0, cx), 0.75, stick_tol, "enhanced") + expect_at(v[1], st(0, cx), 0.25, stick_tol, "quadratic") +
                 expect_at(v[2], st(0, cx), 0.125, stick_tol, "cubic") + expect_at(v[4], st(0, cx), 0.875, stick_tol, "easeOutCubic");
        });
      mk(5, { val(kv(key(5), true), "invert") }, [=](const sig_t &b, const vecs &v) {
        return expect_at(v[0], st(0, cx), -b[st(0, cx)], stick_tol, "inverted x") + expect_at(v[0], st(0, cy), b[st(0, cy)], stick_tol, "y kept");
      });
      mk(6, { val(kv(key(6), true), "invert") }, [=](const sig_t &b, const vecs &v) {
        return expect_at(v[0], st(0, cy), -b[st(0, cy)], stick_tol, "inverted y") + expect_at(v[0], st(0, cx), b[st(0, cx)], stick_tol, "x kept");
      });
    }

    constexpr double axis_tol = 1.5;

    /** @brief Index of depth @p d (20, 64, 128, 200, 255 -> 0..4) and component @p c (0 left axis, 1 right axis, 2 L2 bit, 3 R2 bit). */
    inline std::size_t
    ta(std::size_t d, std::size_t c) { return d * 4 + c; }

    inline void
    trigger_axis_cases(builder_t &builder, bool left, int first_key) {
      const std::size_t axis = left ? 0 : 1;
      const std::size_t bit = left ? 2 : 3;
      const auto mine = [=](std::size_t i) { return i % 4 == axis || i % 4 == bit; };
      const auto mk = [&](int key, std::vector<value_case_t> values, std::function<std::string(const sig_t &, const vecs &)> body, json setup = json::object()) {
        builder.simple(key, trigger_axis_probe, std::move(values), [=](const sig_t &b, const vecs &v) {
          std::string why = body(b, v);
          for (const auto &sig : v) why += only(b, sig, mine, "the other trigger");
          return why; }, std::move(setup));
      };
      mk(first_key, { val(kv(first_key, 0.3), "0.3") }, [=](const sig_t &, const vecs &v) {
        return expect_at(v[0], ta(2, axis), 74, axis_tol, "deadzone 0.3 at 128") + expect_at(v[0], ta(1, axis), 0, axis_tol, "deadzone 0.3 at 64");
      });
      mk(first_key + 1, { val(kv(first_key + 1, 0.9), "0.9") }, [=](const sig_t &, const vecs &v) {
        return expect_at(v[0], ta(2, bit), 0, 0, "bit at 128 with threshold 0.9") + expect_at(v[0], ta(3, bit), 0, 0, "bit at 200") + expect_at(v[0], ta(4, bit), 1, 0, "bit at 255");
      });
      mk(first_key + 2, { val(kv(first_key + 2, true), "binary") }, [=](const sig_t &, const vecs &v) {
        return expect_at(v[0], ta(1, axis), 0, 0, "axis at 64") + expect_at(v[0], ta(3, axis), 255, 0, "axis at 200") + expect_at(v[0], ta(3, bit), 1, 0, "bit at 200");
      });
      const int base = first_key + 3;
      mk(left ? 56 : 60, { val(kv(left ? 56 : 60, 0.2), "0.2") }, [=](const sig_t &, const vecs &v) {
        return expect_at(v[0], ta(1, axis), 16, axis_tol, "range min 0.2 at 64") + expect_at(v[0], ta(0, axis), 0, axis_tol, "range min 0.2 at 20");
      });
      mk(left ? 57 : 61, { val(kv(left ? 57 : 61, 0.6), "0.6") }, [=](const sig_t &, const vecs &v) {
        return expect_at(v[0], ta(2, axis), 213, axis_tol, "range max 0.6 at 128") + expect_at(v[0], ta(3, axis), 255, axis_tol, "range max 0.6 at 200");
      });
      mk(left ? 58 : 62, { val(kv(left ? 58 : 62, 0.3), "0.3") }, [=](const sig_t &, const vecs &v) {
        return expect_at(v[0], ta(0, axis), 91, axis_tol + 0.5, "anti-deadzone 0.3 at 20");
      });
      mk(left ? 59 : 63, { val(kv(left ? 59 : 63, "quadratic"), "quadratic"), val(kv(left ? 59 : 63, "cubic"), "cubic") }, [=](const sig_t &, const vecs &v) {
        return expect_at(v[0], ta(2, axis), 64, axis_tol, "quadratic at 128") + expect_at(v[1], ta(2, axis), 32, axis_tol, "cubic at 128");
      });
      (void) base;
    }

    constexpr double gyro_tol = 3.5;
  }  // namespace cases_detail

  inline void
  append_input_cases(std::vector<case_t> &table) {
    using namespace cases_detail;
    builder_t builder { table };
    const auto simple = [&](int key, std::function<sig_t(rig_t &)> probe, std::vector<value_case_t> values, auto expect, json setup = json::object()) {
      builder.simple(key, std::move(probe), std::move(values), expect, std::move(setup));
    };

    simple(35, stick_probe, { val(kv(35, "radial"), "radial") }, [](const sig_t &, const vecs &v) {
      return expect_at(v[0], st(0, 0), 0.331, stick_tol, "radial x") + expect_at(v[0], st(0, 1), -0.166, stick_tol, "radial y");
    },
      { { key_name(36), 0.3 }, { key_name(43), 0.3 } });
    stick_cases(builder, 0, 1, 36);
    stick_cases(builder, 2, 3, 43);
    // Keys 36..42 and 43..49 above use first_key + 0..6.
    trigger_axis_cases(builder, true, 50);
    trigger_axis_cases(builder, false, 53);

    // ---- gyro (64-73): [A1 xyz, A2 xyz, settled xyz, small xyz] in deg/s ----
    const auto zero_all = [](const sig_t &s) {
      for (const auto x : s)
        if (std::fabs(x) > 0.2) return false;
      return true;
    };
    // always, aim and mouse were removed: the host already asks for motion at allocation, and aiming or mouse gating needs the client.
    simple(64, gyro_probe, { val(kv(64, "off"), "off") },
      [=](const sig_t &, const vecs &v) { return zero_all(v[0]) ? std::string {} : std::string("gyro not zero while off; "); });
    simple(65, gyro_probe, { val(kv(65, 2.0), "x2") }, [](const sig_t &, const vecs &v) {
      return expect_at(v[0], 6, 400, gyro_tol, "settled x with sensitivity 2") + expect_at(v[0], 7, -200, gyro_tol, "settled y");
    });
    simple(66, gyro_probe, { val(kv(66, 0.5), "0.5"), val(kv(66, 0.0), "0") }, [=](const sig_t &, const vecs &v) {
      std::string why;
      // 0.5 of the 2048-count range at 16.4 counts per deg/s is 62.4 deg/s; the 229 deg/s signal keeps its excess, the 22 deg/s one is gone.
      why += expect_at(v[0], 6, 145.5, gyro_tol, "deadzone 0.5 keeps the excess of the settled signal");
      why += expect_at(v[0], 9, 0, gyro_tol, "deadzone 0.5 silences a 22 deg/s signal");
      why += expect_at(v[1], 9, 20, gyro_tol, "deadzone 0 passes a 20 deg/s signal");
      return why;
    });
    simple(67, gyro_probe, { val(kv(67, "none"), "none"), val(kv(67, "oneEuro"), "oneEuro") }, [](const sig_t &b, const vecs &v) {
      return expect_at(v[0], 0, 200, gyro_tol, "no filter, first sample") + expect_at(b, 0, 106, gyro_tol + 3, "low-pass first sample");
    });
    simple(68, gyro_probe, { val(kv(68, 240.0), "240Hz"), val(kv(68, 1.0), "1Hz") }, [](const sig_t &, const vecs &v) {
      return expect_at(v[0], 0, 199.5, gyro_tol, "240 Hz first sample") + expect_at(v[1], 0, 5, gyro_tol, "1 Hz first sample");
    });
    simple(69, gyro_probe, { val(kv(69, 0.0), "beta 0"), val(kv(69, 1.0), "beta 1", false) }, [](const sig_t &b, const vecs &v) { return expect_at(v[0], 0, 106, gyro_tol + 3, "beta 0 behaves like the plain low-pass") + expect_at(b, 0, 200, gyro_tol, "beta 0.05 with a large step"); }, kv(67, "oneEuro"));
    simple(70, gyro_probe, { val(kv(70, "screen"), "screen"), val(kv(70, "world"), "world"), val({ { key_name(70), "custom" }, { key_name(71), 90 } }, "custom yaw 90") },
      [](const sig_t &, const vecs &v) {
        return expect_at(v[0], 6, 100, gyro_tol, "screen x") + expect_at(v[0], 7, 200, gyro_tol, "screen y") + expect_at(v[1], 6, 200, gyro_tol, "world x") +
               expect_at(v[1], 7, 50, gyro_tol, "world y") + expect_at(v[2], 6, 100, gyro_tol, "custom yaw 90 x");
      });
    simple(71, gyro_probe, { val(kv(71, 90), "yaw 90") }, [](const sig_t &, const vecs &v) { return expect_at(v[0], 6, 100, gyro_tol, "yaw 90 x") + expect_at(v[0], 7, 200, gyro_tol, "yaw 90 y"); }, kv(70, "custom"));
    simple(72, gyro_probe, { val(kv(72, 90), "pitch 90") }, [](const sig_t &, const vecs &v) { return expect_at(v[0], 6, 50, gyro_tol, "pitch 90 x") + expect_at(v[0], 8, -200, gyro_tol, "pitch 90 z"); }, kv(70, "custom"));
    simple(73, gyro_probe, { val(kv(73, 90), "roll 90") }, [](const sig_t &, const vecs &v) { return expect_at(v[0], 7, -50, gyro_tol, "roll 90 y") + expect_at(v[0], 8, -100, gyro_tol, "roll 90 z"); }, kv(70, "custom"));

    // ---- touchpad (74-80): [output, report x, report flags, move x, move y, scroll, right press, right release, click button, consume, click release] ----
    simple(74, touch_probe, { val(kv(74, "mouse"), "mouse"), val(kv(74, "off"), "off"), val(kv(74, "precision"), "precision") }, [](const sig_t &b, const vecs &v) {
      std::string why;
      if (b[0] != 0 || b[1] < 0) why += "default is not the controller touch report; ";
      if (v[0][0] != 1 || v[0][3] == 0) why += "mouse did not move the pointer; ";
      if (v[1][0] != 3) why += "off did not resolve to 3; ";
      if (v[2][0] != 2) why += "precision did not resolve to 2; ";
      return why;
    });
    simple(75, touch_probe, { val(kv(75, "hostPointer"), "hostPointer"), val(kv(75, "precision"), "precision") }, [](const sig_t &, const vecs &v) {
      return (v[0][0] == 1 && v[0][3] != 0 && v[1][0] == 2) ? std::string {} : std::string("output enum does not resolve to 1 / 2; ");
    });
    const json host = kv(75, "hostPointer");
    simple(76, touch_probe, { val(kv(76, 2.0), "x2") }, [](const sig_t &b, const vecs &v) { return expect_at(v[0], 3, b[3] * 2, 1.5, "pointer travel with sensitivity 2"); }, host);
    simple(77, touch_probe, { val(kv(77, 2.0), "x2") }, [](const sig_t &b, const vecs &v) { return expect_at(v[0], 5, b[5] * 2, 2, "scroll with speed 2"); }, host);
    simple(78, touch_probe, { val(kv(78, true), "reversed") }, [](const sig_t &b, const vecs &v) { return expect_at(v[0], 5, -b[5], 1, "scroll reversed"); }, host);
    simple(79, touch_probe, { val(kv(79, "left"), "left"), val(kv(79, "right"), "right"), val(kv(79, "none"), "none") }, [](const sig_t &b, const vecs &v) {
      std::string why;
      if (b[8] != 0 || b[9] != 0) why += "default click is not left to the controller; ";
      if (v[0][8] != 1 || v[0][10] != 1) why += "left click not mapped to button 1; ";
      if (v[1][8] != 3 || v[1][10] != 3) why += "right click not mapped to button 3; ";
      if (v[2][8] != 0 || v[2][9] != 1) why += "none should consume the click without a button; ";
      return why; }, host);
    simple(80, touch_probe, { val(kv(80, "rightClick"), "rightClick"), val(kv(80, "none"), "none") }, [](const sig_t &b, const vecs &v) {
      std::string why;
      if (b[5] == 0 || b[6] != 0) why += "default two-finger gesture is not a scroll; ";
      if (v[0][6] != 1 || v[0][7] != 1) why += "rightClick did not press and release button 3; ";
      if (v[1][5] != 0 || v[1][6] != 0) why += "none still acted; ";
      return why; }, host);
  }

  /** @brief The keys that must say unavailable: key, a valid value, the probe that must not move, why in short. */
  struct unavailable_t {
    int key;
    json valid;
    std::function<sig_t(rig_t &)> probe;
    std::string where;
  };

  inline std::vector<unavailable_t>
  unavailable_keys() {
    return {
      { 31, 1.5, trigger_probe({ 0x04 }), "slope trigger block 0x04" },
      { 96, "host", ext_probe, "output extension has no microphone-route field" },
      { 97, "+6dB", ext_probe, "output extension carries no preamp field" },
      { 98, "headsetOnly", ext_probe, "Bluetooth audio is a client radio decision" },
      { 102, true, rumble_probe, "needs a tuning control channel peer" },
      { 103, true, rumble_probe, "needs a tuning control channel peer" },
      { 104, 30, rumble_probe, "needs a tuning control channel peer" },
      { 10, "rumble", ext_probe, "client side (client tests)" },
      { 100, "legacy", ext_probe, "client side (client tests)" },
      { 101, 100, ext_probe, "client side (client tests)" },
      { 105, "controller2", ext_probe, "client side (client tests)" },
    };
  }
}  // namespace ds5::matrix
