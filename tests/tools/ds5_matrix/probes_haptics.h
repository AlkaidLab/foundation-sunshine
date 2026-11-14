/**
 * @file tests/tools/ds5_matrix/probes_haptics.h
 * @brief PCM, authored-haptics (IR v2) and legacy rumble probes.
 *
 * The stream layer turns the PCM messages of the native path into IR v2 frames or legacy rumble with
 * haptics::authored_ir_session_t and haptics::legacy_rumble_session_t.  These probes feed those same
 * classes with a synthetic signal and a synthetic clock, so the timing keys are exact and need no
 * real time.
 */
#pragma once

#include <chrono>
#include <cmath>
#include <cstring>

#include "src/haptics/authored_ir.h"
#include "tests/tools/ds5_matrix/probes_io.h"

namespace ds5::matrix {
  struct tone_t {
    double hz;
    double amplitude;  ///< 0..1 of full scale
  };

  /** @brief Interleaved two-lane 48 kHz PCM made of sine tones per lane. */
  inline std::vector<std::int16_t>
  synth(std::size_t frames, const std::vector<tone_t> &lane0, const std::vector<tone_t> &lane1, std::size_t start_frame = 0) {
    std::vector<std::int16_t> pcm(frames * 2);
    const double two_pi = 6.283185307179586;
    for (std::size_t frame = 0; frame < frames; ++frame) {
      const double t = static_cast<double>(start_frame + frame) / 48000.0;
      double a = 0, b = 0;
      for (const auto &tone : lane0) a += tone.amplitude * std::sin(two_pi * tone.hz * t);
      for (const auto &tone : lane1) b += tone.amplitude * std::sin(two_pi * tone.hz * t);
      pcm[frame * 2] = static_cast<std::int16_t>(std::lround(std::clamp(a, -1.0, 1.0) * 32767.0));
      pcm[frame * 2 + 1] = static_cast<std::int16_t>(std::lround(std::clamp(b, -1.0, 1.0) * 32767.0));
    }
    return pcm;
  }

  inline float
  wire_float(const haptics::authored_ir_v2_wire_t &wire, std::size_t at) {
    float value;
    std::memcpy(&value, wire.data() + at, sizeof value);
    return value;
  }

  struct ir_frame_t {
    float rms[2] {};
    float peak[2] {};
    float low_ratio[2] {};
  };

  /** @brief The IR v2 frames the stream layer would send for @p pcm (240-frame chunks). */
  inline std::vector<ir_frame_t>
  ir_frames(const std::vector<std::int16_t> &pcm) {
    std::vector<ir_frame_t> out;
    haptics::authored_ir_session_t session;
    if (!session.ready()) return out;
    const std::size_t chunks = pcm.size() / 2 / 240;
    for (std::size_t chunk = 0; chunk < chunks; ++chunk) {
      const auto *bytes = reinterpret_cast<const std::uint8_t *>(pcm.data() + chunk * 480);
      const auto wire = session.process(0, chunk == 0 ? 0x01 : 0x00, 240, static_cast<std::uint32_t>(chunk),
        chunk * 5000ull, std::span<const std::uint8_t> { bytes, 240 * 4 });
      ir_frame_t frame;
      if (wire) {
        for (std::size_t lane = 0; lane < 2; ++lane) {
          frame.rms[lane] = wire_float(*wire, 24 + lane * 20);
          frame.peak[lane] = wire_float(*wire, 28 + lane * 20);
          frame.low_ratio[lane] = wire_float(*wire, 36 + lane * 20);
        }
      }
      out.push_back(frame);
    }
    return out;
  }

  inline double
  mean_tail(const std::vector<ir_frame_t> &frames, float (ir_frame_t::*field)[2], int lane, std::size_t from) {
    double sum = 0;
    std::size_t count = 0;
    for (std::size_t i = from; i < frames.size(); ++i, ++count) sum += (frames[i].*field)[lane];
    return count ? std::round(sum / count * 1e4) / 1e4 : -1.0;
  }

  /** @brief Steady tones for half a second: mean RMS, peak and low-band ratio of lane 0, RMS of lane 1. */
  inline std::function<sig_t(rig_t &)>
  ir_steady_probe(std::vector<tone_t> lane0, std::vector<tone_t> lane1) {
    return [lane0, lane1](rig_t &) {
      const auto frames = ir_frames(synth(240 * 100, lane0, lane1));
      if (frames.empty()) return sig_t { -1 };
      return sig_t { mean_tail(frames, &ir_frame_t::rms, 0, 40), mean_tail(frames, &ir_frame_t::peak, 0, 40),
        mean_tail(frames, &ir_frame_t::low_ratio, 0, 40), mean_tail(frames, &ir_frame_t::rms, 1, 40) };
    };
  }

  /** @brief A loud tone that starts at once: lane-0 RMS of the first twelve chunks. */
  inline std::function<sig_t(rig_t &)>
  ir_onset_probe(std::vector<tone_t> lane) {
    return [lane](rig_t &) {
      const auto frames = ir_frames(synth(240 * 24, lane, lane));
      sig_t out;
      for (std::size_t i = 0; i < 12; ++i) out.push_back(i < frames.size() ? std::round(frames[i].rms[0] * 1e5) / 1e5 : -1.0);
      return out;
    };
  }

  struct emit_t {
    double ms;
    int low;
    int high;
  };

  /** @brief What the legacy rumble fallback emits for @p pcm on a synthetic clock (5 ms per chunk). */
  inline std::vector<emit_t>
  legacy_run(const std::vector<std::int16_t> &pcm, int poll_ms = 0) {
    std::vector<emit_t> out;
    haptics::legacy_rumble_session_t session;
    if (!session.ready()) return out;
    const auto t0 = std::chrono::steady_clock::now();
    const std::size_t chunks = pcm.size() / 2 / 240;
    for (std::size_t chunk = 0; chunk < chunks; ++chunk) {
      const auto now = t0 + std::chrono::microseconds(5000 * chunk);
      const auto *bytes = reinterpret_cast<const std::uint8_t *>(pcm.data() + chunk * 480);
      const auto rumble = session.process(0, chunk == 0 ? 0x01 : 0x00, 240, static_cast<std::uint32_t>(chunk), chunk * 5000ull,
        std::span<const std::uint8_t> { bytes, 240 * 4 }, now);
      if (rumble) out.push_back({ chunk * 5.0, rumble->low_frequency, rumble->high_frequency });
    }
    const auto last = t0 + std::chrono::microseconds(5000 * (chunks - 1));
    for (int ms = 1; ms <= poll_ms; ++ms) {
      const auto rumble = session.poll(last + std::chrono::milliseconds(ms));
      if (rumble) {
        out.push_back({ chunks * 5.0 - 5.0 + ms, rumble->low_frequency, rumble->high_frequency });
        break;
      }
    }
    return out;
  }

  inline std::vector<std::int16_t>
  legacy_burst(std::size_t loud_chunks, std::size_t silent_chunks) {
    auto pcm = synth(240 * loud_chunks, { { 80, 0.6 }, { 300, 0.3 } }, { { 80, 0.6 }, { 300, 0.3 } });
    pcm.resize(pcm.size() + 240 * 2 * silent_chunks, 0);
    return pcm;
  }

  /** @brief A 20 ms pulse then silence: when the last non-zero value goes out and when zero does. */
  inline sig_t
  legacy_hold_probe(rig_t &) {
    const auto emits = legacy_run(legacy_burst(4, 80));
    double last_nonzero = -1, first_zero = -1;
    for (const auto &e : emits) {
      if (e.low || e.high)
        last_nonzero = e.ms;
      else if (last_nonzero >= 0 && first_zero < 0)
        first_zero = e.ms;
    }
    return { last_nonzero, first_zero, static_cast<double>(emits.size()) };
  }

  /** @brief Two seconds of steady tone: number of messages sent. */
  inline sig_t
  legacy_rate_probe(rig_t &) {
    return { static_cast<double>(legacy_run(legacy_burst(400, 0)).size()) };
  }

  /** @brief Eight seconds of steady tone: zero messages and when the first one is sent. */
  inline sig_t
  legacy_duration_probe(rig_t &) {
    const auto emits = legacy_run(legacy_burst(1600, 0));
    double zeros = 0, first = -1;
    for (const auto &e : emits) {
      if (e.low == 0 && e.high == 0 && e.ms > 0) {
        ++zeros;
        if (first < 0) first = e.ms;
      }
    }
    return { zeros, first };
  }

  /** @brief Input stops after 100 ms: milliseconds until the watchdog sends zero. */
  inline sig_t
  legacy_watchdog_probe(rig_t &) {
    const auto emits = legacy_run(legacy_burst(20, 0), 1500);
    if (emits.empty()) return { -1 };
    const auto &last = emits.back();
    return { (last.low == 0 && last.high == 0) ? last.ms - 95.0 : -1.0 };
  }

  /** @brief PCM through the pad: whether any signal arrived and the peak of each lane (a 150 Hz tone of 0.5 and 0.25). */
  inline sig_t
  pcm_probe(rig_t &rig) {
    const auto pcm = synth(240 * 20, { { 150, 0.5 } }, { { 150, 0.25 } });
    const auto messages = rig.play(pcm);
    double peak0 = 0, peak1 = 0;
    for (const auto &message : messages) {
      for (std::size_t i = 0; i + 1 < message.pcm.size(); i += 2) {
        peak0 = std::max(peak0, std::fabs(static_cast<double>(message.pcm[i])));
        peak1 = std::max(peak1, std::fabs(static_cast<double>(message.pcm[i + 1])));
      }
    }
    return { (peak0 > 0 || peak1 > 0) ? 1.0 : 0.0, peak0, peak1 };
  }
}  // namespace ds5::matrix
