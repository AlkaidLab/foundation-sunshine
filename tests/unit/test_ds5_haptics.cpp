/**
 * @file tests/unit/test_ds5_haptics.cpp
 * @brief Output-path tests for the host authored-haptics and ERM reduction.
 */
#include "src/ds5/tuning_runtime.h"
#include "src/haptics/authored_ir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <span>
#include <vector>

namespace {
  using haptics::authored_ir_session_t;
  using haptics::authored_ir_v2_wire_t;
  using haptics::legacy_rumble_session_t;
  using steady_clock_t = std::chrono::steady_clock;

  constexpr float sample_rate = 48000.0f;
  constexpr std::uint16_t chunk_frames = 240;

  class Ds5HapticsTest: public ::testing::Test {
  protected:
    void
    SetUp() override {
      reset_tuning();
    }

    void
    TearDown() override {
      reset_tuning();
    }

    static void
    reset_tuning() {
      auto &state = ds5::tuning::runtime::state();
      state.haptics_mode.store(0u);
      state.hd_low_band_db.store(0.0f);
      state.hd_high_band_db.store(0.0f);
      state.hd_crossover_hz.store(160.0f);
      state.hd_noise_gate.store(0.002f);
      state.hd_latency_ms.store(10u);
      state.hd_smoothing_ms.store(2u);
      state.ir_intensity.store(0.8f);
      state.ir_sharpness.store(0.5f);
      state.rumble_enabled.store(true);
      state.rumble_strength.store(1.0f);
      state.rumble_curve.store(1.0f);
      state.rumble_noise_gate.store(0.020f);
      state.rumble_low_gain.store(1.0f);
      state.rumble_high_gain.store(1.0f);
      state.rumble_hold_ms.store(60u);
      state.rumble_send_interval_ms.store(20u);
      state.rumble_watchdog_ms.store(100u);
      state.rumble_max_duration_ms.store(5000u);
      state.rumble_max_rate_hz.store(250u);
    }

    static void
    deterministic_tuning() {
      auto &state = ds5::tuning::runtime::state();
      state.haptics_mode.store(0u);
      state.hd_low_band_db.store(0.0f);
      state.hd_high_band_db.store(0.0f);
      state.hd_crossover_hz.store(160.0f);
      state.hd_noise_gate.store(0.0f);
      state.hd_latency_ms.store(0u);
      state.hd_smoothing_ms.store(0u);
      state.ir_intensity.store(1.0f);
      state.ir_sharpness.store(0.5f);
      state.rumble_hold_ms.store(60u);
      state.rumble_send_interval_ms.store(20u);
      state.rumble_watchdog_ms.store(100u);
      state.rumble_max_duration_ms.store(0u);
      state.rumble_max_rate_hz.store(250u);
    }

    static std::vector<std::uint8_t>
    sine(float hz, float amplitude, float second_hz = 0.0f, float second_amplitude = 0.0f) {
      std::vector<std::uint8_t> result(chunk_frames * 4u);
      auto *samples = reinterpret_cast<std::int16_t *>(result.data());
      for (std::uint16_t frame = 0; frame < chunk_frames; ++frame) {
        const auto time = static_cast<float>(frame) / sample_rate;
        auto value = amplitude * std::sin(2.0f * std::numbers::pi_v<float> * hz * time);
        if (second_hz > 0.0f) {
          value += second_amplitude * std::sin(2.0f * std::numbers::pi_v<float> * second_hz * time);
        }
        const auto raw = static_cast<std::int16_t>(std::lround(
          std::clamp(value, -1.0f, 1.0f) * 32767.0f));
        samples[frame * 2u] = raw;
        samples[frame * 2u + 1u] = raw;
      }
      return result;
    }

    static std::vector<std::uint8_t>
    silence() {
      return std::vector<std::uint8_t>(chunk_frames * 4u, 0);
    }

    static float
    wire_float(const authored_ir_v2_wire_t &wire, std::size_t offset) {
      const auto bits = static_cast<std::uint32_t>(wire[offset]) |
                        (static_cast<std::uint32_t>(wire[offset + 1]) << 8) |
                        (static_cast<std::uint32_t>(wire[offset + 2]) << 16) |
                        (static_cast<std::uint32_t>(wire[offset + 3]) << 24);
      return std::bit_cast<float>(bits);
    }

    static std::optional<authored_ir_v2_wire_t>
    process(authored_ir_session_t &session, const std::vector<std::uint8_t> &pcm,
      std::uint8_t flags = 1u) {
      return session.process(0, flags, chunk_frames, 1u, 0u,
        std::span<const std::uint8_t> { pcm.data(), pcm.size() });
    }
  };
}  // namespace

TEST_F(Ds5HapticsTest, LowAndHighBandGainsChangeRmsAndRatio) {
  deterministic_tuning();
  auto &state = ds5::tuning::runtime::state();
  const auto low_pcm = sine(100.0f, 0.5f);

  authored_ir_session_t baseline;
  const auto baseline_low = process(baseline, low_pcm);
  ASSERT_TRUE(baseline_low);
  const auto baseline_rms = wire_float(*baseline_low, 24);
  const auto baseline_ratio = wire_float(*baseline_low, 36);
  EXPECT_GT(baseline_rms, 0.05f);

  state.hd_low_band_db.store(12.0f);
  authored_ir_session_t boosted;
  const auto boosted_low = process(boosted, low_pcm);
  ASSERT_TRUE(boosted_low);
  EXPECT_GT(wire_float(*boosted_low, 24), baseline_rms);
  EXPECT_GT(wire_float(*boosted_low, 36), baseline_ratio);

  state.hd_low_band_db.store(0.0f);
  const auto high_pcm = sine(300.0f, 0.5f);
  authored_ir_session_t high_baseline;
  const auto baseline_high = process(high_baseline, high_pcm);
  ASSERT_TRUE(baseline_high);
  state.hd_high_band_db.store(12.0f);
  authored_ir_session_t high_boosted;
  const auto boosted_high = process(high_boosted, high_pcm);
  ASSERT_TRUE(boosted_high);
  EXPECT_GT(wire_float(*boosted_high, 24), wire_float(*baseline_high, 24));
  EXPECT_LT(wire_float(*boosted_high, 36), wire_float(*baseline_high, 36));
}

TEST_F(Ds5HapticsTest, CrossoverChangesTheReportedBandRatio) {
  deterministic_tuning();
  auto &state = ds5::tuning::runtime::state();
  const auto pcm = sine(80.0f, 0.4f, 250.0f, 0.4f);
  state.hd_crossover_hz.store(100.0f);
  authored_ir_session_t low_crossover;
  const auto low = process(low_crossover, pcm);
  ASSERT_TRUE(low);
  state.hd_crossover_hz.store(300.0f);
  authored_ir_session_t high_crossover;
  const auto high = process(high_crossover, pcm);
  ASSERT_TRUE(high);
  EXPECT_NE(wire_float(*low, 36), wire_float(*high, 36));
}

TEST_F(Ds5HapticsTest, NoiseGateAttenuatesLowLevelInput) {
  deterministic_tuning();
  auto &state = ds5::tuning::runtime::state();
  const auto pcm = sine(100.0f, 0.001f);
  authored_ir_session_t open;
  const auto ungated = process(open, pcm);
  ASSERT_TRUE(ungated);
  const auto ungated_rms = wire_float(*ungated, 24);
  EXPECT_GT(ungated_rms, 0.0f);

  state.hd_noise_gate.store(0.02f);
  authored_ir_session_t closed;
  const auto gated = process(closed, pcm);
  ASSERT_TRUE(gated);
  EXPECT_LT(wire_float(*gated, 24), ungated_rms * 0.25f);
}

TEST_F(Ds5HapticsTest, IntensityAndSharpnessChangeTheIrWire) {
  deterministic_tuning();
  auto &state = ds5::tuning::runtime::state();
  const auto pcm = sine(100.0f, 0.4f, 300.0f, 0.3f);
  authored_ir_session_t full;
  const auto full_wire = process(full, pcm);
  ASSERT_TRUE(full_wire);
  const auto full_rms = wire_float(*full_wire, 24);
  const auto full_ratio = wire_float(*full_wire, 36);

  state.ir_intensity.store(0.25f);
  authored_ir_session_t quiet;
  const auto quiet_wire = process(quiet, pcm);
  ASSERT_TRUE(quiet_wire);
  EXPECT_NEAR(wire_float(*quiet_wire, 24), full_rms * 0.25f, 1e-3f);
  EXPECT_NEAR(wire_float(*quiet_wire, 28), wire_float(*full_wire, 28) * 0.25f, 1e-3f);

  state.ir_intensity.store(1.0f);
  state.ir_sharpness.store(1.0f);
  authored_ir_session_t sharp;
  const auto sharp_wire = process(sharp, pcm);
  ASSERT_TRUE(sharp_wire);
  EXPECT_LT(wire_float(*sharp_wire, 36), full_ratio);
}

TEST_F(Ds5HapticsTest, ModeOffStopsIrAndLegacyOutput) {
  deterministic_tuning();
  ds5::tuning::runtime::state().haptics_mode.store(3u);
  const auto pcm = sine(100.0f, 0.8f);
  authored_ir_session_t ir;
  EXPECT_FALSE(process(ir, pcm));
  legacy_rumble_session_t rumble;
  const auto off = rumble.process(0, 1u, chunk_frames, 1u, 0u,
    std::span<const std::uint8_t> { pcm.data(), pcm.size() }, steady_clock_t::now());
  EXPECT_FALSE(off);
}

TEST_F(Ds5HapticsTest, LatencyDelaysTheFirstAnalysisWindow) {
  deterministic_tuning();
  auto &state = ds5::tuning::runtime::state();
  const auto pcm = sine(100.0f, 0.5f);
  state.hd_latency_ms.store(0u);
  authored_ir_session_t immediate;
  const auto first = process(immediate, pcm);
  ASSERT_TRUE(first);
  EXPECT_GT(wire_float(*first, 24), 0.0f);

  state.hd_latency_ms.store(20u);
  authored_ir_session_t delayed;
  const auto delayed_first = process(delayed, pcm);
  ASSERT_TRUE(delayed_first);
  EXPECT_FLOAT_EQ(wire_float(*delayed_first, 24), 0.0f);
}

TEST_F(Ds5HapticsTest, SmoothingMsRampsBandGainChanges) {
  deterministic_tuning();
  auto &state = ds5::tuning::runtime::state();
  const auto pcm = sine(100.0f, 0.3f);

  authored_ir_session_t snapped;
  ASSERT_TRUE(process(snapped, pcm));
  state.hd_low_band_db.store(12.0f);
  state.hd_smoothing_ms.store(0u);
  const auto snapped_wire = process(snapped, pcm);
  ASSERT_TRUE(snapped_wire);

  state.hd_low_band_db.store(0.0f);
  state.hd_smoothing_ms.store(2u);
  authored_ir_session_t smoothed;
  ASSERT_TRUE(process(smoothed, pcm));
  state.hd_low_band_db.store(12.0f);
  state.hd_smoothing_ms.store(20u);
  const auto smoothed_wire = process(smoothed, pcm);
  ASSERT_TRUE(smoothed_wire);
  EXPECT_LT(wire_float(*smoothed_wire, 24), wire_float(*snapped_wire, 24));
}

TEST_F(Ds5HapticsTest, LegacyRumbleHonoursSendIntervalWatchdogAndHold) {
  deterministic_tuning();
  auto &state = ds5::tuning::runtime::state();
  const auto pcm = sine(100.0f, 0.8f);
  state.rumble_send_interval_ms.store(50u);
  legacy_rumble_session_t rate;
  const auto start = steady_clock_t::now();
  const auto first = rate.process(0, 1u, chunk_frames, 1u, 0u,
    std::span<const std::uint8_t> { pcm.data(), pcm.size() }, start);
  ASSERT_TRUE(first);
  EXPECT_TRUE(first->low_frequency != 0 || first->high_frequency != 0);
  EXPECT_FALSE(rate.process(0, 0u, chunk_frames, 2u, 5000u,
    std::span<const std::uint8_t> { pcm.data(), pcm.size() }, start + std::chrono::milliseconds(10)));
  EXPECT_TRUE(rate.process(0, 0u, chunk_frames, 3u, 10000u,
    std::span<const std::uint8_t> { pcm.data(), pcm.size() }, start + std::chrono::milliseconds(60)));
  EXPECT_FALSE(rate.poll(start + std::chrono::milliseconds(80)));
  const auto stopped = rate.poll(start + std::chrono::milliseconds(170));
  ASSERT_TRUE(stopped);
  EXPECT_EQ(stopped->low_frequency, 0u);
  EXPECT_EQ(stopped->high_frequency, 0u);

  state.rumble_send_interval_ms.store(20u);
  state.rumble_hold_ms.store(200u);
  legacy_rumble_session_t held;
  const auto held_start = steady_clock_t::now();
  ASSERT_TRUE(held.process(0, 1u, chunk_frames, 1u, 0u,
    std::span<const std::uint8_t> { pcm.data(), pcm.size() }, held_start));
  const auto trailing = held.process(0, 0u, chunk_frames, 2u, 5000u,
    std::span<const std::uint8_t> { silence().data(), chunk_frames * 4u }, held_start + std::chrono::milliseconds(30));
  ASSERT_TRUE(trailing);
  EXPECT_TRUE(trailing->low_frequency != 0 || trailing->high_frequency != 0);
}

TEST_F(Ds5HapticsTest, LegacyRumbleMaxDurationClearsARunawayEffect) {
  deterministic_tuning();
  auto &state = ds5::tuning::runtime::state();
  state.rumble_max_duration_ms.store(10u);
  const auto pcm = sine(100.0f, 0.8f);
  legacy_rumble_session_t rumble;
  const auto start = steady_clock_t::now();
  ASSERT_TRUE(rumble.process(0, 1u, chunk_frames, 1u, 0u,
    std::span<const std::uint8_t> { pcm.data(), pcm.size() }, start));
  const auto expired = rumble.process(0, 0u, chunk_frames, 2u, 10000u,
    std::span<const std::uint8_t> { pcm.data(), pcm.size() }, start + std::chrono::milliseconds(20));
  ASSERT_TRUE(expired);
  EXPECT_EQ(expired->low_frequency, 0u);
  EXPECT_EQ(expired->high_frequency, 0u);
}

TEST_F(Ds5HapticsTest, LegacyRumbleMaxRateCapsTheOutput) {
  deterministic_tuning();
  auto &state = ds5::tuning::runtime::state();
  state.rumble_send_interval_ms.store(4u);
  state.rumble_max_rate_hz.store(10u);
  const auto pcm = sine(100.0f, 0.8f);
  legacy_rumble_session_t rumble;
  const auto start = steady_clock_t::now();
  ASSERT_TRUE(rumble.process(0, 1u, chunk_frames, 1u, 0u,
    std::span<const std::uint8_t> { pcm.data(), pcm.size() }, start));
  EXPECT_FALSE(rumble.process(0, 0u, chunk_frames, 2u, 5000u,
    std::span<const std::uint8_t> { pcm.data(), pcm.size() }, start + std::chrono::milliseconds(30)));
  EXPECT_TRUE(rumble.process(0, 0u, chunk_frames, 3u, 10000u,
    std::span<const std::uint8_t> { pcm.data(), pcm.size() }, start + std::chrono::milliseconds(110)));
}

TEST_F(Ds5HapticsTest, LegacyRumbleSwitchOffStopsAndReenableResumes) {
  deterministic_tuning();
  auto &state = ds5::tuning::runtime::state();
  const auto pcm = sine(60.0f, 0.5f, 320.0f, 0.5f);
  legacy_rumble_session_t rumble;
  const auto start = steady_clock_t::now();
  const auto first = rumble.process(0, 1u, chunk_frames, 1u, 0u,
    std::span<const std::uint8_t> { pcm.data(), pcm.size() }, start);
  ASSERT_TRUE(first);
  EXPECT_TRUE(first->low_frequency != 0 || first->high_frequency != 0);

  // Switching rumble off stops the motors even though the audio keeps coming,
  // and the stop packet is not swallowed by the send-rate limiter.
  state.rumble_enabled.store(false);
  const auto stopped = rumble.process(0, 0u, chunk_frames, 2u, 5000u,
    std::span<const std::uint8_t> { pcm.data(), pcm.size() }, start + std::chrono::milliseconds(25));
  ASSERT_TRUE(stopped);
  EXPECT_EQ(stopped->low_frequency, 0u);
  EXPECT_EQ(stopped->high_frequency, 0u);
  EXPECT_FALSE(rumble.process(0, 0u, chunk_frames, 3u, 10000u,
    std::span<const std::uint8_t> { pcm.data(), pcm.size() }, start + std::chrono::milliseconds(50)));
  EXPECT_FALSE(rumble.poll(start + std::chrono::milliseconds(200)));

  // Turning it back on resumes from a closed gate; no held pulse from before
  // the switch leaks into the new run.
  state.rumble_enabled.store(true);
  const auto again = rumble.process(0, 0u, chunk_frames, 4u, 15000u,
    std::span<const std::uint8_t> { pcm.data(), pcm.size() }, start + std::chrono::milliseconds(75));
  ASSERT_TRUE(again);
  EXPECT_TRUE(again->low_frequency != 0 || again->high_frequency != 0);
}

TEST_F(Ds5HapticsTest, LegacyRumbleChannelGainsScaleEachMotor) {
  deterministic_tuning();
  auto &state = ds5::tuning::runtime::state();
  const auto pcm = sine(60.0f, 0.5f, 320.0f, 0.5f);

  legacy_rumble_session_t baseline;
  const auto base = baseline.process(0, 1u, chunk_frames, 1u, 0u,
    std::span<const std::uint8_t> { pcm.data(), pcm.size() }, steady_clock_t::now());
  ASSERT_TRUE(base);
  ASSERT_NE(base->low_frequency, 0u);
  ASSERT_NE(base->high_frequency, 0u);

  state.rumble_low_gain.store(0.5f);
  legacy_rumble_session_t low_cut;
  const auto cut_low = low_cut.process(0, 1u, chunk_frames, 1u, 0u,
    std::span<const std::uint8_t> { pcm.data(), pcm.size() }, steady_clock_t::now());
  ASSERT_TRUE(cut_low);
  EXPECT_LT(cut_low->low_frequency, base->low_frequency);
  EXPECT_EQ(cut_low->high_frequency, base->high_frequency);

  state.rumble_low_gain.store(1.0f);
  state.rumble_high_gain.store(0.5f);
  legacy_rumble_session_t high_cut;
  const auto cut_high = high_cut.process(0, 1u, chunk_frames, 1u, 0u,
    std::span<const std::uint8_t> { pcm.data(), pcm.size() }, steady_clock_t::now());
  ASSERT_TRUE(cut_high);
  EXPECT_EQ(cut_high->low_frequency, base->low_frequency);
  EXPECT_LT(cut_high->high_frequency, base->high_frequency);
}
