/**
 * @file src/haptics/authored_ir.cpp
 * @brief Host-side authored DualSense PCM analysis and IR v2 serialization.
 *
 * The configurable LR4 split and per-band gain follow the LPF/HPF biquad
 * forms in ds5-rbj-audio-eq-cookbook-20261031; the analysis engine and its
 * fixed tactile band are described in ds5-moonlight-audio-haptics-provenance-
 * 20261031.  The tuning ranges and application points are from section 16.4.1
 * of docs/architecture/ds5-platform-design.zh.md (commit 29f7673fb).
 */
#include "authored_ir.h"
#include "authored_input.h"
#include "wire.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <numbers>

#include <moonlight_haptics/authored_haptics.h>

#include "src/ds5/tuning_runtime.h"

namespace haptics {
  namespace {
    constexpr std::uint8_t source_stream_start = 0x01;
    constexpr std::uint8_t source_stream_end = 0x02;
    constexpr std::uint8_t source_discontinuity = 0x04;
    std::int16_t
    read_i16(const std::uint8_t *p) {
      return static_cast<std::int16_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8));
    }

    std::int16_t
    to_i16(float value) {
      return static_cast<std::int16_t>(std::lround(std::clamp(value, -1.0f, 1.0f) * 32767.0f));
    }

    /**
     * @brief One transposed-direct-form-II biquad.
     *
     * A Linkwitz-Riley fourth-order split is two identical Butterworth
     * second-order sections (Q = 1/sqrt(2)) for each branch.  The coefficients
     * are the LPF/HPF forms in ds5-rbj-audio-eq-cookbook-20261031; only the
     * host side uses them.
     */
    struct biquad_t {
      float b0 { 1.0f };
      float b1 { 0.0f };
      float b2 { 0.0f };
      float a1 { 0.0f };
      float a2 { 0.0f };
      float z1 { 0.0f };
      float z2 { 0.0f };

      void
      reset() noexcept {
        z1 = 0.0f;
        z2 = 0.0f;
      }

      float
      process(float value) noexcept {
        const auto output = b0 * value + z1;
        z1 = b1 * value - a1 * output + z2;
        z2 = b2 * value - a2 * output;
        return output;
      }

      void
      lowpass(float cutoff_hz, float sample_rate) noexcept {
        constexpr float q = 0.70710678f;
        const auto w0 = 2.0f * std::numbers::pi_v<float> * cutoff_hz / sample_rate;
        const auto cosine = std::cos(w0);
        const auto alpha = std::sin(w0) / (2.0f * q);
        const auto a0 = 1.0f + alpha;
        b0 = (1.0f - cosine) * 0.5f / a0;
        b1 = (1.0f - cosine) / a0;
        b2 = b0;
        a1 = -2.0f * cosine / a0;
        a2 = (1.0f - alpha) / a0;
      }

      void
      highpass(float cutoff_hz, float sample_rate) noexcept {
        constexpr float q = 0.70710678f;
        const auto w0 = 2.0f * std::numbers::pi_v<float> * cutoff_hz / sample_rate;
        const auto cosine = std::cos(w0);
        const auto alpha = std::sin(w0) / (2.0f * q);
        const auto a0 = 1.0f + alpha;
        b0 = (1.0f + cosine) * 0.5f / a0;
        b1 = -(1.0f + cosine) / a0;
        b2 = b0;
        a1 = -2.0f * cosine / a0;
        a2 = (1.0f - alpha) / a0;
      }
    };

    struct lane_state_t {
      std::array<biquad_t, 2> low;
      std::array<biquad_t, 2> high;
      float gate_envelope { 0.0f };
      float gate_gain { 1.0f };
      float gate_hold_seconds { 0.0f };
      bool gate_open { true };
      float low_energy { 0.0f };
      float high_energy { 0.0f };

      void
      reset_filters() noexcept {
        for (auto &section : low) section.reset();
        for (auto &section : high) section.reset();
      }
    };
  }  // namespace

  struct authored_ir_session_t::analysis_state_t {
    static constexpr float sample_rate = 48000.0f;
    static constexpr std::size_t max_frames = 240;
    // One extra frame keeps the read pointer behind the write pointer at the
    // 50 ms maximum; an equal-sized ring would return the sample just written.
    static constexpr std::size_t delay_samples = 50u * 48u * 2u + 2u;
    std::array<lane_state_t, 2> lanes;
    std::array<std::int16_t, delay_samples> delay {};
    std::size_t delay_write { 0 };
    std::uint32_t delay_ms { 0 };
    float low_gain { 1.0f };
    float high_gain { 1.0f };
    float crossover_hz { 160.0f };
    bool configured { false };

    void
    configure(const ds5::tuning::runtime::haptics_tuning_t &tuning) {
      const auto next_crossover = std::clamp(tuning.crossover_hz, 40.0f, 400.0f);
      if (!configured || std::abs(next_crossover - crossover_hz) > 0.01f) {
        crossover_hz = next_crossover;
        for (auto &lane : lanes) {
          lane.low[0].lowpass(crossover_hz, sample_rate);
          lane.low[1].lowpass(crossover_hz, sample_rate);
          lane.high[0].highpass(crossover_hz, sample_rate);
          lane.high[1].highpass(crossover_hz, sample_rate);
          lane.reset_filters();
        }
        configured = true;
      }
      if (delay_ms != tuning.latency_ms) {
        delay_ms = tuning.latency_ms;
        delay.fill(0);
        delay_write = 0;
      }
    }

    void
    process(const std::uint8_t *pcm, std::size_t frames,
      const ds5::tuning::runtime::haptics_tuning_t &tuning, std::int16_t *output) {
      configure(tuning);
      const auto smoothing = tuning.smoothing_ms == 0u ? 1.0f : 1.0f - std::exp(-1.0f / (0.001f * static_cast<float>(tuning.smoothing_ms) * sample_rate));
      const auto gate_coefficient = 1.0f - std::exp(-1.0f / (0.001f * sample_rate));
      const auto low_tilt = 1.25f - 0.5f * tuning.sharpness;
      const auto high_tilt = 0.75f + 0.5f * tuning.sharpness;
      const auto target_low = tuning.low_gain * low_tilt;
      const auto target_high = tuning.high_gain * high_tilt;
      const auto delay_frames = static_cast<std::size_t>(tuning.latency_ms) * 48u;
      for (auto &lane : lanes) {
        lane.low_energy = 0.0f;
        lane.high_energy = 0.0f;
      }

      for (std::size_t frame = 0; frame < frames; ++frame) {
        std::int16_t delayed[2];
        if (delay_frames != 0u) {
          const auto read = (delay_write + delay.size() - delay_frames * 2u) % delay.size();
          delayed[0] = delay[read];
          delayed[1] = delay[read + 1u];
          delay[delay_write] = read_i16(pcm + frame * 4u);
          delay[delay_write + 1u] = read_i16(pcm + frame * 4u + 2u);
          delay_write = (delay_write + 2u) % delay.size();
        }
        else {
          delayed[0] = read_i16(pcm + frame * 4u);
          delayed[1] = read_i16(pcm + frame * 4u + 2u);
        }

        low_gain += (target_low - low_gain) * smoothing;
        high_gain += (target_high - high_gain) * smoothing;
        for (std::size_t lane_index = 0; lane_index < lanes.size(); ++lane_index) {
          auto &lane = lanes[lane_index];
          const auto input = static_cast<float>(delayed[lane_index]) / 32768.0f;
          const auto low = lane.low[1].process(lane.low[0].process(input));
          const auto high = lane.high[1].process(lane.high[0].process(input));
          const auto low_value = low * low_gain;
          const auto high_value = high * high_gain;
          auto value = low_value + high_value;

          // The gate is a hysteretic, 20 ms hold envelope on the same signal
          // the analyzer sees.  A zero threshold disables it completely.
          lane.gate_envelope = std::max(std::abs(value), lane.gate_envelope * 0.9995f);
          if (tuning.noise_gate <= 0.0f) {
            lane.gate_open = true;
            lane.gate_hold_seconds = 0.0f;
          }
          else if (lane.gate_envelope >= tuning.noise_gate) {
            lane.gate_open = true;
            lane.gate_hold_seconds = 0.020f;
          }
          else if (lane.gate_open) {
            lane.gate_hold_seconds -= 1.0f / sample_rate;
            if (lane.gate_hold_seconds <= 0.0f ||
                lane.gate_envelope < tuning.noise_gate * 0.5f) {
              lane.gate_open = false;
            }
          }
          const auto gate_target = lane.gate_open ? 1.0f : 0.0f;
          lane.gate_gain += (gate_target - lane.gate_gain) * gate_coefficient;
          value *= lane.gate_gain;

          lane.low_energy += low_value * low_value;
          lane.high_energy += high_value * high_value;
          output[frame * 2u + lane_index] = to_i16(value);
        }
      }
    }

    float
    low_band_ratio(std::size_t lane) const {
      const auto total = lanes[lane].low_energy + lanes[lane].high_energy;
      return total > 0.0f ? lanes[lane].low_energy / total : 1.0f;
    }
  };

  void
  authored_ir_session_t::engine_deleter_t::operator()(AhAuthoredEngine *engine) const noexcept {
    ah_authored_destroy(engine);
  }

  authored_ir_session_t::authored_ir_session_t() {
    AhAuthoredConfig config {};
    AhAuthoredEngine *engine = nullptr;
    if (ah_authored_config_init(&config, 48000) == AH_STATUS_OK &&
        ah_authored_create(&config, &engine) == AH_STATUS_OK) {
      _engine.reset(engine);
    }
    _analysis = std::make_unique<analysis_state_t>();
  }

  authored_ir_session_t::~authored_ir_session_t() = default;

  bool
  authored_ir_session_t::ready() const noexcept {
    return _engine != nullptr && _analysis != nullptr;
  }

  std::optional<authored_frame_t>
  authored_ir_session_t::analyze(std::uint8_t source_flags, std::uint16_t frame_count,
    std::uint32_t sequence, std::uint64_t presentation_time_us,
    std::span<const std::uint8_t> pcm) {
    if (!_engine || !_analysis) return std::nullopt;

    const auto tuning = ds5::tuning::runtime::haptics_tuning();
    if (!tuning.enabled) return std::nullopt;

    detail::authored_pcm_t aligned_pcm {};
    const auto prepared = detail::prepare_authored_input(
      source_flags, frame_count, sequence, presentation_time_us, pcm, aligned_pcm);
    if (!prepared) return std::nullopt;
    _analysis->process(pcm.data(), frame_count, tuning, aligned_pcm.data());
    auto input = *prepared;
    input.interleaved_pcm = aligned_pcm.data();

    std::array<AhAuthoredHapticFrame, 1> output {};
    std::uint32_t output_count = 0;
    const auto status = ah_authored_process_i16(
      _engine.get(), &input, output.data(),
      static_cast<std::uint32_t>(output.size()), &output_count);
    if (status != AH_STATUS_OK && status != AH_STATUS_OUTPUT_AVAILABLE) {
      return std::nullopt;
    }

    if (output_count == 0 && (source_flags & source_stream_end)) {
      return authored_frame_t {
        .flags = AH_AUTHORED_FRAME_STREAM_END | AH_AUTHORED_FRAME_SILENT,
        .timestamp_us = presentation_time_us,
        .source_sequence_number = sequence,
        .source_frame_count = frame_count,
        .lanes = {},
        .lane_correlation = 0.0f,
      };
    }
    if (output_count != 1) {
      return std::nullopt;
    }

    const auto &frame = output[0];
    authored_frame_t result {
      .flags = frame.flags,
      .timestamp_us = frame.timestamp_us,
      .source_sequence_number = frame.source_sequence_number,
      .source_frame_count = frame.source_frame_count,
      .lanes = {},
      .lane_correlation = frame.lane_correlation,
    };
    for (std::size_t lane = 0; lane < result.lanes.size(); ++lane) {
      result.lanes[lane] = {
        .rms_amplitude = frame.lanes[lane].rms_amplitude,
        .peak_amplitude = frame.lanes[lane].peak_amplitude,
        .transient_strength = frame.lanes[lane].transient_strength,
        .low_band_ratio = _analysis->low_band_ratio(lane),
        .zero_crossing_rate_hz = frame.lanes[lane].zero_crossing_rate_hz,
      };
    }
    return result;
  }

  std::optional<authored_ir_v2_wire_t>
  authored_ir_session_t::process(std::uint16_t controller_id, std::uint8_t source_flags,
    std::uint16_t frame_count, std::uint32_t sequence,
    std::uint64_t presentation_time_us,
    std::span<const std::uint8_t> pcm) {
    auto analyzed = analyze(source_flags, frame_count, sequence, presentation_time_us, pcm);
    if (!analyzed) return std::nullopt;

    authored_ir_v2_wire_t wire {};
    wire[0] = DS5_HAPTICS_IR_STREAM_PROTOCOL_VERSION;
    haptics::wire::write_u16(wire.data() + 2, authored_ir_v2_wire_size);
    haptics::wire::write_u16(wire.data() + 4, controller_id);
    auto &frame = *analyzed;
    const auto intensity = ds5::tuning::runtime::haptics_tuning().intensity;
    for (auto &lane : frame.lanes) {
      lane.rms_amplitude = std::clamp(lane.rms_amplitude * intensity, 0.0f, 1.0f);
      lane.peak_amplitude = std::clamp(lane.peak_amplitude * intensity, 0.0f, 1.0f);
      lane.transient_strength = std::clamp(lane.transient_strength * intensity, 0.0f, 1.0f);
    }
    if (frame.flags & AH_AUTHORED_FRAME_DISCONTINUITY) wire[1] |= 0x01;
    if (frame.flags & AH_AUTHORED_FRAME_PARTIAL) wire[1] |= 0x02;
    if (frame.flags & AH_AUTHORED_FRAME_STREAM_END) wire[1] |= 0x04;
    if (frame.flags & AH_AUTHORED_FRAME_SILENT) wire[1] |= 0x08;
    haptics::wire::write_u32(wire.data() + 8, frame.source_sequence_number);
    haptics::wire::write_u64(wire.data() + 12, frame.timestamp_us);
    haptics::wire::write_u32(wire.data() + 20, frame.source_frame_count);
    for (std::size_t lane = 0; lane < 2; ++lane) {
      auto *lane_wire = wire.data() + 24 + lane * 20;
      const auto &lane_frame = frame.lanes[lane];
      haptics::wire::write_float(lane_wire, lane_frame.rms_amplitude);
      haptics::wire::write_float(lane_wire + 4, lane_frame.peak_amplitude);
      haptics::wire::write_float(lane_wire + 8, lane_frame.transient_strength);
      haptics::wire::write_float(lane_wire + 12, lane_frame.low_band_ratio);
      haptics::wire::write_float(lane_wire + 16, lane_frame.zero_crossing_rate_hz);
    }
    haptics::wire::write_float(wire.data() + 64, frame.lane_correlation);
    haptics::wire::write_u32(wire.data() + 68, 0);
    return wire;
  }

}  // namespace haptics
