#include "googcc_runtime.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace transport {
  googcc_runtime_t::googcc_runtime_t(googcc_runtime_config_t config, std::shared_ptr<policy_state_t> policy,
    frame_policy_ref_t activation_policy):
      config_(std::move(config)), policy_(std::move(policy)), activation_policy_(std::move(activation_policy)), controller_(config_.controller) {
    if (!policy_ || !config_.feedback_negotiated || config_.feedback_timeout_us <= 0 ||
        config_.policy_interval_us <= 0 ||
        policy_->snapshot().accepted->connection_epoch != config_.controller.connection_epoch) {
      throw std::invalid_argument("Invalid serialized GoogCC runtime");
    }
    if (!activation_policy_) activation_policy_ = policy_->snapshot().accepted;
    if (activation_policy_->connection_epoch != config_.controller.connection_epoch ||
        (activation_policy_->automatic_control &&
          (activation_policy_->automatic_control->bitrate != config_.automatic_bitrate_enabled ||
            activation_policy_->automatic_control->fec ||
            static_cast<std::int64_t>(config_.controller.maximum_kbps) + activation_policy_->budget.other_kbps +
                activation_policy_->budget.repair_kbps + activation_policy_->budget.probe_kbps >
              activation_policy_->automatic_control->maximum_total_kbps)))
      throw std::invalid_argument("Runtime does not match its activation policy");
    collect_probes();
  }

  void
  googcc_runtime_t::collect_probes() {
    for (const auto &request : controller_.take_probe_requests()) {
      if (!config_.budgeted_probing_enabled || !config_.automatic_bitrate_enabled || revoked_ || pending_probes_.size() == 32)
        ++rejected_probes_;
      else
        pending_probes_.push_back(request);
    }
  }

  bool
  googcc_runtime_t::probe_eligible(std::int64_t now_us) const {
    if (!config_.budgeted_probing_enabled || !config_.automatic_bitrate_enabled || !lease_ || revoked_ || !fresh(now_us)) return false;
    const auto state = policy_->snapshot();
    return !state.stopped && state.encoder_initialized && state.accepted &&
           state.accepted->connection_epoch == lease_->connection_epoch &&
           state.accepted->control_epoch == lease_->control_epoch && state.accepted->control_source == lease_->source;
  }

  std::span<const googcc_probe_t>
  googcc_runtime_t::pending_probe_requests(std::int64_t now_us) {
    if (pending_probes_.empty()) return {};
    const auto eligible = probe_eligible(now_us);
    const auto state = policy_->snapshot();
    const bool awaiting_handoff = config_.budgeted_probing_enabled && config_.automatic_bitrate_enabled &&
                                  !granted_ && !revoked_ && !state.stopped;
    rejected_probes_ += std::erase_if(pending_probes_, [&](const auto &request) {
      return request.requested_at_us < 0 || now_us < request.requested_at_us ||
             now_us - request.requested_at_us > 1000000 || (!eligible && !awaiting_handoff);
    });
    return eligible ? std::span<const googcc_probe_t>(pending_probes_) : std::span<const googcc_probe_t>();
  }

  bool
  googcc_runtime_t::consume_probe_request(std::int32_t cluster_id) {
    if (pending_probes_.empty() || pending_probes_.front().cluster_id != cluster_id) return false;
    pending_probes_.erase(pending_probes_.begin());
    return true;
  }

  bool
  googcc_runtime_t::fresh(std::int64_t now_us) const {
    const auto covered = controller_.snapshot().last_covered_feedback_us;
    const auto sent = controller_.snapshot().last_covered_send_us;
    return covered >= 0 && now_us >= covered && now_us - covered <= config_.feedback_timeout_us &&
           sent >= 0 && now_us >= sent && now_us - sent <= config_.feedback_timeout_us;
  }

  bool
  googcc_runtime_t::retain_lease(const frame_policy_ref_t &accepted) {
    if (!lease_) return false;
    if (!accepted || accepted->connection_epoch != lease_->connection_epoch ||
        accepted->control_epoch != lease_->control_epoch || accepted->control_source != lease_->source) {
      lease_.reset();
      revoked_ = true;
      return false;
    }
    return true;
  }

  bool
  googcc_runtime_t::on_successful_send(const successful_send_event_t &event) {
    const auto accepted = controller_.on_successful_send(event);
    collect_probes();
    return accepted;
  }

  bool
  googcc_runtime_t::on_feedback(const feedback_event_t &event) {
    const auto previous_clock_resets = controller_.snapshot().receiver_clock_resets;
    const auto accepted = controller_.on_feedback(event);
    if (controller_.snapshot().receiver_clock_resets != previous_clock_resets) {
      rejected_probes_ += pending_probes_.size();
      pending_probes_.clear();
    }
    collect_probes();
    return accepted;
  }

  bool
  googcc_runtime_t::try_take_control(std::int64_t now_us, std::optional<googcc_queue_sample_t> queue) {
    if (granted_ || revoked_ || !config_.control_negotiated || !config_.deadline_pacing_enabled ||
        (!config_.automatic_bitrate_enabled && !config_.controller.queue_pushback) || !fresh(now_us)) return false;
    const auto state = policy_->snapshot();
    const auto &policy = state.accepted;
    if (policy != activation_policy_) {
      revoked_ = true;
      return false;
    }
    if (state.stopped || !state.encoder_initialized || policy != state.applied ||
        policy->basis != budget_basis_e::normalized ||
        (policy->control_source != control_source_e::manual && policy->control_source != control_source_e::legacy) ||
        policy->connection_epoch != config_.controller.connection_epoch) return false;
    const auto receipt = std::find_if(state.receipts.begin(), state.receipts.end(), [&](const auto &item) { return item.policy == policy; });
    if (receipt == state.receipts.end() || !receipt->encoder_applied || !receipt->first_sent_frame ||
        receipt->failure != policy_failure_e::none) return false;
    if (!controller_.process_interval(now_us, queue)) return false;
    collect_probes();
    const control_lease_t previous { policy->connection_epoch, policy->control_epoch, policy->control_source };
    const auto result = policy_->transfer_control(previous, control_source_e::googcc, policy->revision);
    if (result.result != policy_request_result_e::accepted) return false;
    lease_ = control_lease_t { result.policy->connection_epoch, result.policy->control_epoch, result.policy->control_source };
    granted_ = true;
    stale_ = false;
    last_policy_us_ = now_us;
    return true;
  }

  bool
  googcc_runtime_t::process_interval(std::int64_t now_us, std::optional<googcc_queue_sample_t> queue) {
    if (!controller_.process_interval(now_us, queue)) return false;
    collect_probes();
    stale_ = !fresh(now_us);
    const auto state = policy_->snapshot();
    if (state.stopped) {
      lease_.reset();
      revoked_ = true;
      return true;
    }
    if (!retain_lease(state.accepted)) return true;
    if (last_policy_us_ >= 0 && now_us - last_policy_us_ < config_.policy_interval_us) return true;
    const auto &policy = state.accepted;
    auto budget = policy->budget;
    const auto reserve = static_cast<std::int64_t>(budget.other_kbps) + budget.repair_kbps + budget.probe_kbps;
    const auto current_video = static_cast<std::int64_t>(budget.total_kbps) - reserve;
    // Feedback silence freezes a safe ceiling: it never licenses an upstep.
    const auto estimate = std::clamp<std::int64_t>(controller_.snapshot().target_kbps,
      config_.controller.minimum_kbps, config_.controller.maximum_kbps);
    const auto video_target = !config_.automatic_bitrate_enabled     ? current_video :
                              (stale_ || !state.encoder_initialized) ? std::min(current_video, estimate) :
                                                                       estimate;
    const auto total = video_target + reserve;
    if (total <= 0 || total > 800000 || total > std::numeric_limits<int>::max()) {
      ++rejected_requests_;
      last_policy_us_ = now_us;
      return true;
    }
    budget.total_kbps = static_cast<int>(total);
    const auto base = policy->fec_base, key = policy->fec_key, recovery = policy->fec_recovery;
    last_policy_us_ = now_us;
    auto encoder_ceiling = policy->encoder_ceiling_kbps;
    if (config_.controller.queue_pushback) {
      auto normalized = budget;
      normalized.fec_numerator = std::max({ base, key, recovery });
      normalized.fec_denominator = 100;
      const auto allocation = policy_->allocate_budget(normalized);
      if (!allocation || allocation->encoder_kbps <= 0) {
        ++rejected_requests_;
        return true;
      }
      std::optional<int> desired;
      const auto ratio = controller_.snapshot().encoder_reduce_ratio;
      if (ratio > 0) desired = std::max(1, static_cast<int>(allocation->encoder_kbps * (1 - ratio)));
      // Queue freshness cannot raise an unconfirmed production ceiling. The
      // network budget remains independent so previously queued data can drain.
      if (stale_ || !state.encoder_initialized || state.applied != policy) {
        if (encoder_ceiling && (!desired || *desired > *encoder_ceiling)) desired = encoder_ceiling;
      }
      encoder_ceiling = desired;
    }
    if (budget == policy->budget && encoder_ceiling == policy->encoder_ceiling_kbps) return true;
    const auto result = policy_->request_controller_update(*lease_, budget,
      base, key, recovery, policy->revision, {}, encoder_ceiling);
    if (result.result == policy_request_result_e::accepted) {
      ++accepted_policy_requests_;
    }
    else {
      ++rejected_requests_;
      retain_lease(policy_->snapshot().accepted);
    }
    last_policy_us_ = now_us;
    return true;
  }

  googcc_runtime_snapshot_t
  googcc_runtime_t::snapshot() const {
    return { controller_.snapshot(), lease_, accepted_policy_requests_, rejected_requests_, rejected_probes_, revoked_, stale_ };
  }
  void
  googcc_runtime_t::stop() noexcept {
    rejected_probes_ += pending_probes_.size();
    pending_probes_.clear();
    lease_.reset();
    revoked_ = true;
  }
}  // namespace transport
