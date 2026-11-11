/** Legacy ERM rendering policy; independent of the v2 wire adapter. */
#include "authored_ir.h"
#include "src/ds5/config.h"
#include "src/logging.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <moonlight_haptics/authored_haptics.h>

namespace haptics {
  namespace {
    constexpr std::uint8_t source_stream_start = 0x01;
    constexpr std::uint8_t source_discontinuity = 0x04;
    constexpr auto legacy_emit_period = std::chrono::milliseconds(20);
    // Some clients dispatch rumble on a slower timer and keep only the newest
    // queued packet. Hold short synthesized pulses long enough that a stop
    // packet cannot replace the only nonzero dispatch.
    constexpr auto legacy_min_active_hold = std::chrono::milliseconds(80);
    constexpr auto legacy_watchdog_timeout = std::chrono::milliseconds(100);
    constexpr float legacy_gate_open = 0.020f;
    constexpr float legacy_gate_close = 0.010f;
    constexpr float legacy_gate_hold_seconds = 0.060f;
    constexpr float legacy_output_floor = 0.030f;
    constexpr float legacy_low_band_trim = 1.15f;
    constexpr float legacy_high_band_trim = 1.20f;
    constexpr float legacy_transient_trim = 1.15f;
    constexpr float legacy_low_makeup_gain = 1.35f;
    constexpr float legacy_high_makeup_gain = 1.45f;
    constexpr float legacy_low_attack_seconds = 0.012f;
    constexpr float legacy_low_release_seconds = 0.040f;
    constexpr float legacy_high_attack_seconds = 0.006f;
    constexpr float legacy_high_release_seconds = 0.025f;

    float
    shaped(float value, float makeup_gain, gate_state_t &gate, float duration_seconds,
      float gate_open, float gate_close, float curve, float strength) {
      value = std::clamp(value, 0.0f, 1.0f);
      if (value >= gate_open) {
        gate.open = true;
        gate.hold_seconds = legacy_gate_hold_seconds;
      }
      else if (gate.open) {
        // Hysteresis only bridges short dips. Without the hold budget a level
        // parked between the two thresholds -- which is exactly where residual
        // out-of-band energy lands -- would keep the gate latched forever.
        gate.hold_seconds -= duration_seconds;
        if (value <= gate_close || gate.hold_seconds <= 0.0f) {
          gate.open = false;
        }
      }
      if (!gate.open) return 0.0f;

      const auto gated = std::clamp(
        (value - gate_close) / (1.0f - gate_close), 0.0f, 1.0f);
      // Curve 0.5 is the current stock perceptual mapping. Keep its sqrt()
      // path point-for-point with master while allowing custom exponents.
      const auto drive = curve == 0.5f ? std::sqrt(gated) :
                         curve == 1.0f ? gated :
                                         std::pow(gated, curve);
      return strength * std::tanh(makeup_gain * drive) / std::tanh(makeup_gain);
    }

    std::uint16_t
    rumble_u16(float value) {
      return static_cast<std::uint16_t>(std::lround(
        std::clamp(value, 0.0f, 1.0f) * std::numeric_limits<std::uint16_t>::max()));
    }
  }  // namespace

  bool
  legacy_rumble_session_t::ready() const noexcept {
    return _analyzer.ready();
  }

  std::optional<legacy_rumble_t>
  legacy_rumble_session_t::process(std::uint16_t controller_id, std::uint8_t source_flags,
    std::uint16_t frame_count, std::uint32_t sequence,
    std::uint64_t presentation_time_us,
    std::span<const std::uint8_t> pcm,
    std::chrono::steady_clock::time_point now) {
    const auto frame = _analyzer.analyze(
      source_flags, frame_count, sequence, presentation_time_us, pcm);
    if (!frame) return std::nullopt;

    _controller_id = controller_id;
    _last_input = now;
    _have_input = true;

    const bool force_emit = (source_flags & (source_stream_start | source_discontinuity)) != 0;
    if (force_emit) {
      _smoothed_low = 0.0f;
      _smoothed_high = 0.0f;
      _low_gate = {};
      _high_gate = {};
      _last_emit = {};
      _active_since = {};
      _last_nonzero_low = 0;
      _last_nonzero_high = 0;
      _short_release_low = false;
      _short_release_high = false;
    }

    const bool must_stop = (frame->flags & AH_AUTHORED_FRAME_STREAM_END) != 0;
    float low_energy = 0.0f;
    float high_energy = 0.0f;
    if ((frame->flags & AH_AUTHORED_FRAME_SILENT) == 0) {
      // Trim gains compensate ERM motors' weak response to low drive levels;
      // transients get less because peak amplitude already overshoots RMS.
      for (const auto &lane : frame->lanes) {
        const auto low = lane.rms_amplitude * std::sqrt(std::clamp(lane.low_band_ratio, 0.0f, 1.0f));
        const auto high_band = lane.rms_amplitude * std::sqrt(std::clamp(1.0f - lane.low_band_ratio, 0.0f, 1.0f));
        const auto transient = lane.peak_amplitude * lane.transient_strength;
        low_energy = std::max(low_energy, low * legacy_low_band_trim);
        high_energy = std::max(high_energy, std::max(
                                              high_band * legacy_high_band_trim, transient * legacy_transient_trim));
      }
    }

    // The heavier low motor keeps body slightly longer; the lighter high motor
    // tracks texture and impacts quickly. Time-based coefficients keep this
    // independent of the sidecar's chunk boundaries.
    const auto duration_seconds = static_cast<float>(std::max(frame->source_frame_count, 1u)) / 48000.0f;
    // The immutable snapshot was fully validated before publication. Load it
    // once so both motor lanes use one coherent revision without per-packet
    // file access, parsing, locking, or duplicated range handling.
    const auto settings = ds5_config::current();
    if (_tuning_revision != settings.revision) {
      _low_gate = {};
      _high_gate = {};
      _tuning_revision = settings.revision;
      BOOST_LOG(debug) << "Controller " << controller_id
                       << " adopted legacy haptics tuning revision " << settings.revision;
    }
    const auto gate_open = static_cast<float>(settings.legacy_noise_gate);
    const auto gate_close = gate_open * 0.5f;
    const auto curve = static_cast<float>(settings.legacy_curve);
    const auto strength = static_cast<float>(settings.legacy_strength);
    if (must_stop) {
      _low_gate = {};
      _high_gate = {};
      _short_release_low = false;
      _short_release_high = false;
    }
    const auto low_target = must_stop ? 0.0f : shaped(low_energy, legacy_low_makeup_gain, _low_gate, duration_seconds, gate_open, gate_close, curve, strength);
    const auto high_target = must_stop ? 0.0f : shaped(high_energy, legacy_high_makeup_gain, _high_gate, duration_seconds, gate_open, gate_close, curve, strength);

    // A force clear is only appropriate when a target drops out during the
    // minimum hold window. Effects that were already active beyond that
    // window must use the configured release tail instead.
    const bool within_min_hold =
      _active_since != std::chrono::steady_clock::time_point {} &&
      now - _active_since < legacy_min_active_hold;
    if (must_stop || low_target > 0.0f) {
      _short_release_low = false;
    }
    else if (within_min_hold) {
      _short_release_low = true;
    }
    if (must_stop || high_target > 0.0f) {
      _short_release_high = false;
    }
    else if (within_min_hold) {
      _short_release_high = true;
    }

    const auto smooth = [duration_seconds](float previous, float target,
                          float attack, float release) {
      const auto tau = target > previous ? attack : release;
      const auto alpha = 1.0f - std::exp(-duration_seconds / tau);
      return previous + (target - previous) * alpha;
    };
    _smoothed_low = must_stop ? 0.0f : smooth(_smoothed_low, low_target, legacy_low_attack_seconds, legacy_low_release_seconds);
    _smoothed_high = must_stop ? 0.0f : smooth(_smoothed_high, high_target, legacy_high_attack_seconds, legacy_high_release_seconds);
    const bool hold_expired =
      _active_since != std::chrono::steady_clock::time_point {} &&
      now - _active_since >= legacy_min_active_hold;
    if (hold_expired && _short_release_low && low_target <= 0.0f) _smoothed_low = 0.0f;
    if (hold_expired && _short_release_high && high_target <= 0.0f) _smoothed_high = 0.0f;
    if (low_target <= 0.0f && _smoothed_low < legacy_output_floor) _smoothed_low = 0.0f;
    if (high_target <= 0.0f && _smoothed_high < legacy_output_floor) _smoothed_high = 0.0f;

    auto low = rumble_u16(_smoothed_low);
    auto high = rumble_u16(_smoothed_high);
    if (low != 0 || high != 0) {
      if (_active_since == std::chrono::steady_clock::time_point {}) _active_since = now;
      _last_nonzero_low = low;
      _last_nonzero_high = high;
    }
    else if (!must_stop && _active_since != std::chrono::steady_clock::time_point {} &&
             now - _active_since < legacy_min_active_hold) {
      low = _last_nonzero_low;
      high = _last_nonzero_high;
    }
    else if (must_stop || _active_since != std::chrono::steady_clock::time_point {}) {
      _active_since = {};
      _last_nonzero_low = 0;
      _last_nonzero_high = 0;
      _short_release_low = false;
      _short_release_high = false;
    }
    // The 20 ms rate limit is the only emission gate. Held silence additionally
    // stays quiet instead of re-sending zero rumble at 50 Hz.
    const bool silent_hold = low == 0 && high == 0 && _last_low == 0 && _last_high == 0;
    if (!must_stop && !force_emit &&
        (silent_hold ||
          (_last_emit != std::chrono::steady_clock::time_point {} && now - _last_emit < legacy_emit_period))) {
      return std::nullopt;
    }

    _last_emit = now;
    _last_low = low;
    _last_high = high;
    return legacy_rumble_t { controller_id, low, high };
  }

  std::optional<legacy_rumble_t>
  legacy_rumble_session_t::poll(std::chrono::steady_clock::time_point now) {
    if (!_have_input || now - _last_input < legacy_watchdog_timeout) {
      return std::nullopt;
    }
    const bool had_output = _last_low != 0 || _last_high != 0;
    _have_input = false;
    _smoothed_low = 0.0f;
    _smoothed_high = 0.0f;
    _low_gate = {};
    _high_gate = {};
    _last_low = 0;
    _last_high = 0;
    _active_since = {};
    _last_nonzero_low = 0;
    _last_nonzero_high = 0;
    _short_release_low = false;
    _short_release_high = false;
    if (!had_output) return std::nullopt;
    _last_emit = now;
    return legacy_rumble_t { _controller_id, 0, 0 };
  }
}  // namespace haptics
