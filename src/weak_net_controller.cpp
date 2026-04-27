#include "weak_net_controller.h"

#include <algorithm>
#include <cmath>

namespace weak_net {
  namespace {
    double
    ratio(std::uint32_t numerator, std::uint32_t denominator) {
      if (denominator == 0) {
        return 0.0;
      }
      return static_cast<double>(numerator) / static_cast<double>(denominator);
    }

    double
    ewma(double previous, double sample, double alpha) {
      return previous == 0.0 ? sample : previous * (1.0 - alpha) + sample * alpha;
    }

    int
    clamp_percent(int value, int max_fec_percentage = controller_t::max_fec_percentage) {
      return std::clamp(value, 0, std::clamp(max_fec_percentage, 0, controller_t::max_fec_percentage));
    }

    int
    clamp_fps(int value, int min_fps, int baseline_fps) {
      min_fps = std::clamp(min_fps, 1, std::max(1, baseline_fps));
      return std::clamp(value, min_fps, std::max(min_fps, baseline_fps));
    }

    int
    approach_int(int current, int target, double fraction, int min_step, int max_step) {
      if (current == target) {
        return current;
      }

      const auto delta = target - current;
      auto step = static_cast<int>(std::lround(std::abs(delta) * fraction));
      step = std::clamp(step, min_step, std::max(min_step, max_step));
      step = std::min(step, std::abs(delta));
      return current + (delta > 0 ? step : -step);
    }

    int
    probe_step(int current_bitrate_kbps, int ceiling_bitrate_kbps) {
      const auto remaining = std::max(0, ceiling_bitrate_kbps - current_bitrate_kbps);
      if (remaining == 0) {
        return 0;
      }
      return std::clamp(static_cast<int>(std::lround(remaining * 0.12)), 500, 15000);
    }

    int
    total_bitrate_for_encoding_bitrate(int encoding_bitrate_kbps, int fec_percentage) {
      if (encoding_bitrate_kbps <= 0) {
        return encoding_bitrate_kbps;
      }

      fec_percentage = clamp_percent(fec_percentage);
      return fec_percentage > 0 ?
               static_cast<int>(std::lround(
                 static_cast<double>(encoding_bitrate_kbps) * 100.0 /
                 static_cast<double>(100 - fec_percentage))) :
               encoding_bitrate_kbps;
    }

    int
    encoding_bitrate_for_total_budget(int total_bitrate_kbps, int fec_percentage) {
      if (total_bitrate_kbps <= 0) {
        return total_bitrate_kbps;
      }

      fec_percentage = clamp_percent(fec_percentage);
      return fec_percentage > 0 ?
               static_cast<int>(std::lround(
                 static_cast<double>(total_bitrate_kbps) *
                 static_cast<double>(100 - fec_percentage) / 100.0)) :
               total_bitrate_kbps;
    }

    int
    pacing_budget_for_encoding_bitrate(int encoding_bitrate_kbps, int fec_percentage) {
      if (encoding_bitrate_kbps <= 0) {
        return encoding_bitrate_kbps;
      }

      const auto total_bitrate_kbps = total_bitrate_for_encoding_bitrate(encoding_bitrate_kbps, fec_percentage);
      return static_cast<int>(std::lround(static_cast<double>(total_bitrate_kbps) * 1.04));
    }

    int
    clamp_pacing_budget(int pacing_bitrate_kbps, int ceiling_total_bitrate_kbps, int min_bitrate_kbps) {
      if (ceiling_total_bitrate_kbps > 0) {
        pacing_bitrate_kbps = std::min(pacing_bitrate_kbps, ceiling_total_bitrate_kbps);
      }
      return std::max(std::min(min_bitrate_kbps, std::max(1, ceiling_total_bitrate_kbps)), pacing_bitrate_kbps);
    }
  }  // namespace

  void
  controller_t::configure(config_t config) {
    config_ = config;
    config_.baseline_bitrate_kbps = std::max(config_.baseline_bitrate_kbps, config_.min_bitrate_kbps);
    config_.max_fec_percentage = clamp_percent(config_.max_fec_percentage);
    config_.baseline_fec_percentage = clamp_percent(config_.baseline_fec_percentage, config_.max_fec_percentage);
    if (config_.ceiling_total_bitrate_kbps <= 0) {
      config_.ceiling_total_bitrate_kbps = total_bitrate_for_encoding_bitrate(config_.baseline_bitrate_kbps,
                                                                              config_.baseline_fec_percentage);
    }
    config_.ceiling_total_bitrate_kbps = std::max(config_.ceiling_total_bitrate_kbps, config_.min_bitrate_kbps);
    const auto startup_encoding_limit = std::min(config_.baseline_bitrate_kbps,
                                                 encoding_bitrate_for_total_budget(config_.ceiling_total_bitrate_kbps,
                                                                                   config_.baseline_fec_percentage));
    config_.startup_bitrate_kbps = config_.startup_bitrate_kbps > 0 ?
                                     std::clamp(config_.startup_bitrate_kbps, config_.min_bitrate_kbps, startup_encoding_limit) :
                                     startup_encoding_limit;
    config_.baseline_fps = std::clamp(config_.baseline_fps <= 0 ? 60 : config_.baseline_fps, 1, 240);
    const auto default_min_fps = config_.baseline_fps >= 60 ? 24 :
                                 config_.baseline_fps >= 30 ? 18 :
                                 std::max(1, config_.baseline_fps / 2);
    config_.min_fps = std::clamp(config_.min_fps <= 0 ? default_min_fps : config_.min_fps, 1, config_.baseline_fps);
    config_.startup_fps = config_.startup_fps > 0 ?
                            std::clamp(config_.startup_fps, config_.min_fps, config_.baseline_fps) :
                            config_.baseline_fps;
    current_bitrate_kbps_ = config_.startup_bitrate_kbps;
    current_fec_percentage_ = config_.baseline_fec_percentage;
    pacing_bitrate_kbps_ = clamp_pacing_budget(
      pacing_budget_for_encoding_bitrate(current_bitrate_kbps_, current_fec_percentage_),
      config_.ceiling_total_bitrate_kbps,
      config_.min_bitrate_kbps);
    current_fps_ = config_.startup_fps;
    state_ = state_e::healthy;
    stable_windows_ = 0;
    idr_cooldown_windows_ = 0;
    ewma_loss_ = 0.0;
    ewma_unrecoverable_ = 0.0;
    ewma_jitter_ = 0.0;
    ewma_deadline_pressure_ = 0.0;
    ewma_input_pressure_ = 0.0;
    configured_ = true;
  }

  action_t
  controller_t::on_feedback(const feedback_t &feedback) {
    if (!configured_) {
      configure({});
    }

    const auto lost_packets = feedback.total_packets > feedback.received_packets ?
                                feedback.total_packets - feedback.received_packets :
                                feedback.missing_packets;
    const auto loss = std::max(ratio(lost_packets, feedback.total_packets),
                               ratio(feedback.missing_packets, feedback.total_packets));
    const auto unrecoverable = ratio(feedback.unrecoverable_frames, std::max(feedback.frames_seen, 1U));
    const auto recovered = ratio(feedback.recovered_frames, std::max(feedback.frames_seen, 1U));
    const auto jitter = static_cast<double>(feedback.rtt_variance_ms);
    const auto late = ratio(feedback.late_frames, std::max(feedback.frames_seen, 1U));
    const auto decode_queue = static_cast<double>(feedback.decode_queue_depth);
    const auto render_queue = static_cast<double>(feedback.render_queue_depth);
    const auto input_latency_ms = static_cast<double>(
                                    std::max(feedback.input_send_latency_us, feedback.input_ack_latency_us)) /
                                  1000.0;
    const auto deadline_pressure = std::max({
      late * 3.0,
      decode_queue / 4.0,
      render_queue / 3.0,
      jitter / 90.0,
    });
    const auto input_pressure = std::max(static_cast<double>(feedback.input_queue_depth) / 4.0,
                                         input_latency_ms / 80.0);

    const bool raw_network_clean = loss <= 0.002 &&
                                   unrecoverable == 0.0 &&
                                   recovered <= 0.005 &&
                                   jitter <= 18.0;
    const bool raw_deadline_clean = late == 0.0 &&
                                    decode_queue == 0.0 &&
                                    render_queue == 0.0 &&
                                    input_pressure <= 0.15;

    ewma_loss_ = ewma(ewma_loss_, loss, raw_network_clean ? 0.72 : 0.45);
    ewma_unrecoverable_ = ewma(ewma_unrecoverable_, unrecoverable, raw_network_clean ? 0.72 : 0.55);
    ewma_jitter_ = ewma(ewma_jitter_, jitter, raw_network_clean ? 0.65 : 0.35);
    ewma_deadline_pressure_ = ewma(ewma_deadline_pressure_, deadline_pressure, raw_deadline_clean ? 0.68 : 0.45);
    ewma_input_pressure_ = ewma(ewma_input_pressure_, input_pressure, raw_deadline_clean ? 0.68 : 0.5);

    const auto previous_bitrate = current_bitrate_kbps_;
    const auto previous_fec = current_fec_percentage_;
    const auto previous_fps = current_fps_;
    const auto previous_state = state_;
    const bool raw_deadline_miss = deadline_pressure >= 0.95 || input_pressure >= 1.1;
    const bool hard_deadline_miss = ewma_deadline_pressure_ >= 1.15 || ewma_input_pressure_ >= 1.25;
    const bool raw_network_crisis = unrecoverable >= 0.10 ||
                                    loss >= 0.12 ||
                                    jitter >= 130.0;
    const bool raw_network_constrained = unrecoverable >= 0.015 ||
                                         loss >= 0.03 ||
                                         recovered >= 0.03 ||
                                         jitter >= 45.0;
    const bool network_crisis = raw_network_crisis ||
                                (!raw_network_clean &&
                                 (ewma_unrecoverable_ >= 0.12 ||
                                  ewma_loss_ >= 0.16 ||
                                  ewma_jitter_ >= 110.0));
    const bool network_constrained = raw_network_constrained ||
                                     (!raw_network_clean &&
                                      (ewma_unrecoverable_ >= 0.015 ||
                                       ewma_loss_ >= 0.035 ||
                                       ewma_jitter_ >= 40.0));
    const bool deadline_crisis = ewma_deadline_pressure_ >= 1.7;
    const bool deadline_constrained = raw_deadline_miss || hard_deadline_miss || deadline_crisis;

    if (idr_cooldown_windows_ > 0) {
      --idr_cooldown_windows_;
    }

    if (network_crisis) {
      state_ = state_e::crisis;
      stable_windows_ = 0;
      if (deadline_constrained) {
        current_fps_ = clamp_fps(static_cast<int>(std::lround(current_fps_ * 0.88)), config_.min_fps, config_.baseline_fps);
      }
      current_bitrate_kbps_ = std::max(config_.min_bitrate_kbps, static_cast<int>(std::lround(current_bitrate_kbps_ * 0.76)));
      current_fec_percentage_ = clamp_percent(std::max(current_fec_percentage_ + 4, config_.baseline_fec_percentage + 6),
                                              config_.max_fec_percentage);
    }
    else if (network_constrained) {
      state_ = state_e::constrained;
      stable_windows_ = 0;
      if (deadline_constrained) {
        current_fps_ = clamp_fps(static_cast<int>(std::lround(current_fps_ * 0.92)), config_.min_fps, config_.baseline_fps);
      }
      current_bitrate_kbps_ = std::max(config_.min_bitrate_kbps, static_cast<int>(std::lround(current_bitrate_kbps_ * 0.90)));
      current_fec_percentage_ = clamp_percent(std::max(current_fec_percentage_ + 2, config_.baseline_fec_percentage + 2),
                                              config_.max_fec_percentage);
    }
    else if (deadline_constrained) {
      state_ = state_e::constrained;
      stable_windows_ = 0;
      current_fps_ = clamp_fps(static_cast<int>(std::lround(current_fps_ * (hard_deadline_miss ? 0.92 : 0.94))),
                               config_.min_fps,
                               config_.baseline_fps);
    }
    else {
      stable_windows_++;
      if (current_bitrate_kbps_ < config_.baseline_bitrate_kbps ||
          current_fec_percentage_ > config_.baseline_fec_percentage ||
          current_fps_ < config_.baseline_fps) {
        state_ = state_e::recovering;
        if (stable_windows_ >= 3 && current_fec_percentage_ > config_.baseline_fec_percentage) {
          current_fec_percentage_ = std::max(config_.baseline_fec_percentage,
                                             current_fec_percentage_ - (stable_windows_ >= 7 ? 2 : 1));
        }
        const auto bitrate_recovery_ceiling = std::min(
          config_.baseline_bitrate_kbps,
          encoding_bitrate_for_total_budget(config_.ceiling_total_bitrate_kbps, current_fec_percentage_));
        current_fps_ = approach_int(current_fps_,
                                    config_.baseline_fps,
                                    0.16,
                                    1,
                                    std::max(4, config_.baseline_fps / 12));
        current_bitrate_kbps_ = std::min(bitrate_recovery_ceiling,
                                         current_bitrate_kbps_ + probe_step(current_bitrate_kbps_, bitrate_recovery_ceiling));
      }
      else {
        state_ = state_e::healthy;
      }
    }

    current_bitrate_kbps_ = std::min(
      current_bitrate_kbps_,
      std::min(config_.baseline_bitrate_kbps,
               encoding_bitrate_for_total_budget(config_.ceiling_total_bitrate_kbps, current_fec_percentage_)));
    pacing_bitrate_kbps_ = clamp_pacing_budget(
      pacing_budget_for_encoding_bitrate(current_bitrate_kbps_, current_fec_percentage_),
      config_.ceiling_total_bitrate_kbps,
      config_.min_bitrate_kbps);

    const bool request_idr = network_crisis &&
                             state_ == state_e::crisis &&
                             previous_state != state_e::crisis &&
                             idr_cooldown_windows_ == 0;
    if (request_idr) {
      idr_cooldown_windows_ = 8;
    }

    return {
      .changed = previous_bitrate != current_bitrate_kbps_ ||
                 previous_fec != current_fec_percentage_ ||
                 previous_fps != current_fps_ ||
                 previous_state != state_,
      .state = state_,
      .target_bitrate_kbps = current_bitrate_kbps_,
      .fec_percentage = current_fec_percentage_,
      .pacing_bitrate_kbps = pacing_bitrate_kbps_,
      .target_fps = current_fps_,
      .request_idr = request_idr,
    };
  }

  int
  controller_t::current_bitrate_kbps() const {
    return current_bitrate_kbps_;
  }

  int
  controller_t::current_fec_percentage() const {
    return current_fec_percentage_;
  }

  int
  controller_t::pacing_bitrate_kbps() const {
    return pacing_bitrate_kbps_;
  }

  int
  controller_t::current_fps() const {
    return current_fps_;
  }

  state_e
  controller_t::state() const {
    return state_;
  }
}  // namespace weak_net
