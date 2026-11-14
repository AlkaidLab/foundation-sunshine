/**
 * @file tests/tools/ds5_matrix/probes_io.h
 * @brief Measurements taken at the device boundary: output reports in, feedback messages out,
 *        gamepad state in, input reports out.
 */
#pragma once

#include <cmath>

#include "tests/tools/ds5_matrix/matrix_engine.h"

namespace ds5::matrix {
  /** @brief A 48-byte output report as a game writes it (byte 0 is the report id). */
  inline report48
  out_report(std::uint8_t flag0, std::uint8_t flag1 = 0, std::uint8_t flag2 = 0) {
    report48 report {};
    report[0] = 2;
    report[1] = flag0;
    report[2] = flag1;
    report[39] = flag2;
    return report;
  }

  inline const fb_t *
  last_of(const std::vector<fb_t> &messages, fb_kind kind) {
    const fb_t *found = nullptr;
    for (const auto &message : messages) {
      if (message.kind == kind) found = &message;
    }
    return found;
  }

  /** @brief Rumble as the client receives it: compat-vibration reports of 100/200 and 10/10. */
  inline sig_t
  rumble_probe(rig_t &rig) {
    sig_t out;
    for (const auto &[left, right] : { std::pair<int, int> { 100, 200 }, { 10, 10 } }) {
      auto report = out_report(0x01);
      report[3] = static_cast<std::uint8_t>(right);
      report[4] = static_cast<std::uint8_t>(left);
      const auto *rumble = last_of(rig.output(report), fb_kind::rumble);
      out.push_back(rumble ? rumble->low : -1);
      out.push_back(rumble ? rumble->high : -1);
    }
    return out;
  }

  /** @brief A trigger block of the given mode as a game authors it (11 bytes, mode first). */
  inline std::array<std::uint8_t, 11>
  trigger_block(std::uint8_t mode) {
    std::array<std::uint8_t, 11> block {};
    block[0] = mode;
    switch (mode) {
      case 0x01:
        block[1] = 0x30;
        block[2] = 100;
        break;
      case 0x02:
        block[1] = 0x20;
        block[2] = 0x60;
        block[3] = 100;
        break;
      case 0x06:
        block[1] = 0x20;
        block[2] = 100;
        block[3] = 0x40;
        break;
      case 0x21:
      case 0x26: {
        std::uint32_t packed = 0;
        for (std::uint32_t zone = 0; zone < 10; ++zone) packed |= 3u << (3 * zone);
        block[1] = 0xff;
        block[2] = 0x03;
        for (int i = 0; i < 4; ++i) block[3 + i] = static_cast<std::uint8_t>(packed >> (8 * i));
        block[9] = 20;
        break;
      }
      case 0x25:
        block[1] = 0x02;
        block[2] = 0x07;
        block[3] = 3;
        break;
      default:
        for (int i = 1; i < 11; ++i) block[i] = static_cast<std::uint8_t>(0x10 + i);
        break;
    }
    return block;
  }

  /** @brief Per mode: messages sent, then what the client holds on each side (type and first six parameters). */
  inline std::function<sig_t(rig_t &)>
  trigger_probe(std::vector<std::uint8_t> modes) {
    return [modes](rig_t &rig) {
      sig_t out;
      for (const auto mode : modes) {
        auto flush = out_report(0x0c);
        rig.output(flush);
        auto report = out_report(0x0c);
        const auto block = trigger_block(mode);
        std::copy(block.begin(), block.end(), report.begin() + 11);
        std::copy(block.begin(), block.end(), report.begin() + 22);
        std::size_t count = 0;
        for (const auto &message : rig.output(report)) count += message.kind == fb_kind::trigger ? 1 : 0;
        const auto &view = rig.trigger_view();
        out.push_back(static_cast<double>(count));
        out.push_back(view.left_type);
        out.push_back(view.right_type);
        for (int i = 0; i < 6; ++i) out.push_back(view.left[i]);
        for (int i = 0; i < 6; ++i) out.push_back(view.right[i]);
      }
      return out;
    };
  }

  /** @brief Lightbar colour for two game updates. */
  inline sig_t
  lightbar_probe(rig_t &rig) {
    sig_t out;
    for (const auto &colour : { std::array<int, 3> { 200, 100, 50 }, { 255, 255, 255 } }) {
      auto report = out_report(0, 0x04);
      for (int i = 0; i < 3; ++i) report[45 + i] = static_cast<std::uint8_t>(colour[i]);
      const auto *rgb = last_of(rig.output(report), fb_kind::rgb);
      out.push_back(rgb ? rgb->r : -1);
      out.push_back(rgb ? rgb->g : -1);
      out.push_back(rgb ? rgb->b : -1);
    }
    return out;
  }

  /** @brief Messages and final colour when the session ends after a game colour of 200/100/50. */
  inline sig_t
  release_probe(rig_t &rig) {
    auto report = out_report(0, 0x04);
    report[45] = 200;
    report[46] = 100;
    report[47] = 50;
    rig.output(report);
    const auto ended = rig.end_session();
    const auto *last = last_of(ended, fb_kind::rgb);
    return { static_cast<double>(ended.size()), last ? double(last->r) : -1.0, last ? double(last->g) : -1.0, last ? double(last->b) : -1.0 };
  }

  /** @brief A game report that sets every extension field. */
  inline report48
  ext_report(std::uint8_t headset, std::uint8_t speaker, std::uint8_t mic, std::uint8_t route, std::uint8_t mute_led,
    bool mic_mute, std::uint8_t brightness, std::uint8_t player) {
    auto report = out_report(0xf0, 0x13, 0x01);
    report[5] = headset;
    report[6] = speaker;
    report[7] = mic;
    report[8] = route;
    report[9] = mute_led;
    report[10] = mic_mute ? 0x10 : 0x00;
    report[43] = brightness;
    report[44] = player;
    return report;
  }

  inline sig_t
  ext_view_sig(const ext_view_t &view) {
    return { double(view.player), double(view.mute), double(view.speaker), double(view.headset), double(view.mic),
      double(view.mic_mute), double(view.route), double(view.brightness) };
  }

  /** @brief What the client holds after two rounds of game reports that set every extension field. */
  inline sig_t
  ext_probe(rig_t &rig) {
    rig.output(ext_report(0x50, 0x70, 0x30, 0, 1, true, 2, 0x1f));
    rig.output(ext_report(0x20, 0x50, 0x10, 2, 0, false, 1, 0x04));
    auto out = ext_view_sig(rig.ext_view());
    rig.output(ext_report(0x40, 0x40, 0x40, 3, 0, false, 5, 0x1f));
    rig.output(ext_report(0x30, 0x60, 0x20, 0, 1, true, 3, 0x0a));
    const auto second = ext_view_sig(rig.ext_view());
    out.insert(out.end(), second.begin(), second.end());
    return out;
  }

  inline double
  stick_norm(std::uint8_t byte, bool y) {
    const double v = (static_cast<double>(byte) - 127.5) / 127.5;
    return std::round((y ? -v : v) * 1000.0) / 1000.0;
  }

  /** @brief Both sticks for four deflections: normalised x,y of the left then the right stick. */
  inline sig_t
  stick_probe(rig_t &rig) {
    sig_t out;
    for (const auto &[x, y] : { std::pair<int, int> { 16384, -8192 }, { 2000, 0 }, { 32767, 32767 }, { -23170, 23170 } }) {
      pad_state_t state;
      state.lsx = state.rsx = static_cast<std::int16_t>(x);
      state.lsy = state.rsy = static_cast<std::int16_t>(y);
      const auto report = rig.input(state);
      out.push_back(stick_norm(report[1], false));
      out.push_back(stick_norm(report[2], true));
      out.push_back(stick_norm(report[3], false));
      out.push_back(stick_norm(report[4], true));
    }
    return out;
  }

  /** @brief Both trigger axes and the digital L2/R2 bits at five pull depths. */
  inline sig_t
  trigger_axis_probe(rig_t &rig) {
    sig_t out;
    for (const int depth : { 20, 64, 128, 200, 255 }) {
      pad_state_t state;
      state.lt = state.rt = static_cast<std::uint8_t>(depth);
      const auto report = rig.input(state);
      out.push_back(report[5]);
      out.push_back(report[6]);
      out.push_back((report[9] >> 2) & 1);
      out.push_back((report[9] >> 3) & 1);
    }
    return out;
  }

  /** @brief Gyro in deg/s: step response after two and three samples, settled value, a small signal. */
  inline sig_t
  gyro_probe(rig_t &rig) {
    sig_t out;
    const auto dps = [&](const report64 &r, std::size_t at) {
      return std::round(static_cast<std::int16_t>(r[at] | (r[at + 1] << 8)) / 16.4 * 10.0) / 10.0;
    };
    const auto push = [&](const report64 &r) {
      for (std::size_t at : { 16u, 18u, 20u }) out.push_back(dps(r, at));
    };
    for (int i = 0; i < 800; ++i) rig.gyro(0, 0, 0, false);
    push(rig.gyro(200, -100, 50, true));
    push(rig.gyro(200, -100, 50, true));
    for (int i = 0; i < 60; ++i) rig.gyro(200, -100, 50, false);
    push(rig.gyro(200, -100, 50, true));
    for (int i = 0; i < 30; ++i) rig.gyro(20, 10, 0, false);
    push(rig.gyro(20, 10, 0, true));
    return out;
  }
}  // namespace ds5::matrix
