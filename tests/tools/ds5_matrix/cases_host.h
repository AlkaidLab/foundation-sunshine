/**
 * @file tests/tools/ds5_matrix/cases_host.h
 * @brief Cases for the preset, the haptics keys 2 to 12 and the legacy rumble timing keys 19 to 23, and rumble 13 to 18.
 */
#pragma once

#include "tests/tools/ds5_matrix/cases_common.h"

namespace ds5::matrix {
  inline void
  append_host_cases(std::vector<case_t> &table) {
    using namespace cases_detail;
    builder_t builder { table };
    const auto add = [&](case_t c) { builder.add(std::move(c)); };
    const auto simple = [&](int key, std::function<sig_t(rig_t &)> probe, std::vector<value_case_t> values, auto expect, json setup = json::object()) {
      builder.simple(key, std::move(probe), std::move(values), expect, std::move(setup));
    };
    // ---- preset (1) ----
    {
      case_t c;
      c.key = 1;
      c.probe = preset_probe;
      c.values = { val(kv(1, "competitive"), "competitive"), val(kv(1, "immersive"), "immersive"), val(kv(1, "legacy"), "legacy") };
      c.range_check = false;
      c.expect = [](const sig_t &base, const vecs &v) {
        std::string why;
        if (v.size() < 3 || v[0].size() < 8) return std::string("probe missing");
        // sig: rumble lo, hi, light r g b, pcm chunks, pcm peak0, trigger type
        if (v[0][0] != 0) why += want("competitive rumble", v[0][0], 0);
        if (!near(v[0][3], 42, 1) || !near(v[0][4], 89, 1)) why += "competitive lightbar not fixed 0/42/89; ";
        if (!near(v[0][6], base[6] * 0.6, 40)) why += want("competitive pcm peak", v[0][6], base[6] * 0.6);
        if (!near(v[1][0], std::min(65535.0, base[0] * 1.3), 120)) why += want("immersive rumble", v[1][0], base[0] * 1.3);
        if (!near(v[1][6], std::min(32767.0, base[6] * 1.4), 40)) why += want("immersive pcm peak", v[1][6], base[6] * 1.4);
        if (v[2][5] != 0) why += want("legacy pcm chunks", v[2][5], 0);
        if (v[2][7] != 5) why += want("legacy trigger type", v[2][7], 5);
        return why;
      };
      c.extra = [](rig_t &rig, std::string &detail) {
        std::string why;
        rig.post(kv(1, "competitive"));
        if (snapshot_value(rig, 1) != "competitive") why += "GET does not report preset competitive after saving it; ";
        rig.post(kv(14, 1.7));
        if (snapshot_value(rig, 1) != "custom") why += "changing a bundle key did not turn the preset into custom; ";
        const auto bad = rig.post(kv(1, "custom"));
        if (bad.ok || bad.http_status != 400) why += "preset custom was accepted; ";
        const auto unknown = rig.post(kv(1, "nonsense"));
        if (unknown.ok || unknown.http_status != 400) why += "unknown preset was accepted; ";
        rig.post(merged(kv(1, "competitive"), kv(13, true)));
        if (rumble_probe(rig)[0] == 0) why += "explicit value in the same request did not win over the bundle; ";
        detail += " | preset GET/custom/invalid/explicit-wins checked";
        return why;
      };
      add(std::move(c));
    }

    // ---- haptics (2-9, 11, 12) ----
    // authoredPcm and ir were removed: the route follows what the client offers, so they changed nothing.
    simple(2, pcm_probe, { val(kv(2, "off"), "off") },
      [](const sig_t &, const vecs &v) { return v[0][0] != 0 ? want("off: pcm chunks", v[0][0], 0) : std::string {}; });
    simple(3, pcm_probe, { val(kv(3, 0.5), "x0.5"), val(kv(3, 2.0), "x2.0") },
      [](const sig_t &b, const vecs &v) {
        std::string why;
        if (!near(v[0][1], b[1] * 0.5, 4) || !near(v[0][2], b[2] * 0.5, 4)) why += "gain 0.5 not halving the peaks; ";
        if (!near(v[1][2], b[2] * 2, 4)) why += "gain 2.0 not doubling lane 1; ";
        if (v[1][1] < 32767) why += want("gain 2.0 lane 0 clip", v[1][1], 32767);
        return why;
      });
    const std::vector<tone_t> low_high { { 60, 0.4 }, { 400, 0.4 } };
    const std::vector<tone_t> low_high_b { { 60, 0.2 }, { 400, 0.2 } };
    simple(4, ir_steady_probe(low_high, low_high_b), { val(kv(4, 12.0), "+12dB"), val(kv(4, -24.0), "-24dB") },
      [](const sig_t &b, const vecs &v) { return v[0][2] > b[2] ? std::string {} : std::string("low band share did not rise with +12 dB; "); });
    simple(5, ir_steady_probe(low_high, low_high_b), { val(kv(5, 12.0), "+12dB"), val(kv(5, -24.0), "-24dB") },
      [](const sig_t &b, const vecs &v) { return v[0][2] < b[2] ? std::string {} : std::string("low band share did not fall with +12 dB high band; "); });
    simple(6, ir_steady_probe({ { 100, 0.5 } }, { { 100, 0.5 } }), { val(kv(6, 40.0), "40Hz"), val(kv(6, 400.0), "400Hz") },
      [](const sig_t &b, const vecs &v) { return v[0][2] < b[2] ? std::string {} : std::string("crossover 40 Hz did not move a 100 Hz tone to the high band; "); });
    simple(7, ir_steady_probe({ { 80, 0.03 } }, { { 80, 0.03 } }), { val(kv(7, 0.06), "gate 0.06") },
      [](const sig_t &b, const vecs &v) { return v[0][0] < b[0] * 0.2 ? std::string {} : std::string("gate 0.06 did not mute a 0.03 tone; "); });
    simple(8, ir_onset_probe({ { 120, 0.8 } }), { val(kv(8, 2), "2ms"), val(kv(8, 50), "50ms") }, [](const sig_t &b, const vecs &v) {
      std::string why;
      if (!(v[0][0] > b[0])) why += "2 ms did not start sooner than 10 ms; ";
      if (!(v[1][5] < b[5])) why += "50 ms did not start later than 10 ms; ";
      return why;
    });
    simple(9, ir_onset_probe({ { 120, 0.8 } }), { val(kv(9, 20), "20ms"), val(kv(9, 0), "0ms") }, [](const sig_t &, const vecs &) { return std::string {}; }, kv(4, 12.0));
    simple(11, ir_steady_probe(low_high, low_high_b), { val(kv(11, 0.4), "0.4"), val(kv(11, 0.0), "0") },
      [](const sig_t &b, const vecs &v) {
        std::string why;
        if (!near(v[0][0], b[0] * 0.5, b[0] * 0.03)) why += want("intensity 0.4 rms", v[0][0], b[0] * 0.5);
        if (v[1][0] != 0) why += want("intensity 0 rms", v[1][0], 0);
        return why;
      });
    simple(12, ir_steady_probe(low_high, low_high_b), { val(kv(12, 1.0), "1.0"), val(kv(12, 0.0), "0.0") },
      [](const sig_t &, const vecs &) { return std::string {}; });

    // ---- rumble (13-18) ----
    simple(13, rumble_probe, { val(kv(13, false), "off") }, [](const sig_t &b, const vecs &v) {
      for (const auto x : v[0])
        if (x != 0) return std::string("rumble still sent while disabled: ") + fmt(v[0]);
      return b[0] == 0 ? std::string("default rumble is zero; ") : std::string {};
    });
    simple(14, rumble_probe, { val(kv(14, 0.5), "x0.5"), val(kv(14, 4.0), "x4") }, [](const sig_t &b, const vecs &v) {
      std::string why;
      for (std::size_t i = 0; i < 4; ++i)
        if (!near(v[0][i], b[i] * 0.5, 2)) why += want("strength 0.5", v[0][i], b[i] * 0.5);
      if (v[1][1] != 65535) why += want("strength 4 saturation", v[1][1], 65535);
      return why;
    });
    simple(15, rumble_probe, { val(kv(15, 2.0), "curve2"), val(kv(15, 0.5), "curve0.5") }, [](const sig_t &, const vecs &v) {
      std::string why;
      if (!near(v[0][0], std::pow(100.0 / 255.0, 2.0) * 65535.0, 3)) why += want("curve 2 low", v[0][0], std::pow(100.0 / 255.0, 2.0) * 65535.0);
      if (!near(v[0][1], std::pow(200.0 / 255.0, 2.0) * 65535.0, 3)) why += want("curve 2 high", v[0][1], std::pow(200.0 / 255.0, 2.0) * 65535.0);
      return why;
    });
    simple(16, rumble_probe, { val(kv(16, 0.5), "low x0.5") }, [](const sig_t &b, const vecs &v) {
      std::string why;
      if (!near(v[0][0], b[0] * 0.5, 2) || !near(v[0][2], b[2] * 0.5, 2)) why += "low motor not halved; ";
      if (v[0][1] != b[1] || v[0][3] != b[3]) why += "high motor moved; ";
      return why;
    });
    simple(17, rumble_probe, { val(kv(17, 0.5), "high x0.5") }, [](const sig_t &b, const vecs &v) {
      std::string why;
      if (!near(v[0][1], b[1] * 0.5, 2) || !near(v[0][3], b[3] * 0.5, 2)) why += "high motor not halved; ";
      if (v[0][0] != b[0] || v[0][2] != b[2]) why += "low motor moved; ";
      return why;
    });
    simple(18, rumble_probe, { val(kv(18, 0.06), "gate 0.06") }, [](const sig_t &b, const vecs &v) {
      std::string why;
      if (v[0][2] != 0 || v[0][3] != 0) why += "10/255 still passes a gate of 0.06; ";
      if (v[0][0] != b[0] || v[0][1] != b[1]) why += "loud rumble changed; ";
      return why;
    });
    simple(19, legacy_hold_probe, { val(kv(19, 200), "200ms"), val(kv(19, 0), "0ms") }, [](const sig_t &b, const vecs &v) {
      return v[0][0] > b[0] ? std::string {} : want("hold 200 last non-zero ms", v[0][0], b[0] + 1);
    });
    simple(20, legacy_rate_probe, { val(kv(20, 50), "50ms"), val(kv(20, 4), "4ms") }, [](const sig_t &b, const vecs &v) {
      return (v[0][0] < b[0] && v[1][0] > b[0]) ? std::string {} : std::string("message count does not follow the send interval; ");
    });
    simple(21, legacy_watchdog_probe, { val(kv(21, 500), "500ms"), val(kv(21, 20), "20ms") }, [](const sig_t &, const vecs &v) {
      std::string why;
      if (!near(v[0][0], 500, 8)) why += want("watchdog 500", v[0][0], 500);
      if (!near(v[1][0], 20, 8)) why += want("watchdog 20", v[1][0], 20);
      return why;
    });
    simple(22, legacy_duration_probe, { val(kv(22, 1000), "1000ms"), val(kv(22, 0), "0=off") }, [](const sig_t &b, const vecs &v) {
      std::string why;
      if (!(v[0][0] > b[0])) why += "max duration 1000 did not cut more often; ";
      if (v[1][0] != 0) why += want("max duration 0 cuts", v[1][0], 0);
      return why;
    });
    simple(23, legacy_rate_probe, { val(kv(23, 25), "25Hz") }, [](const sig_t &b, const vecs &v) {
      return v[0][0] < b[0] ? std::string {} : std::string("max rate 25 Hz did not reduce the message count; ");
    });
  }
}  // namespace ds5::matrix
