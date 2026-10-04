#include "transport_policy.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace transport {
  policy_state_t::policy_state_t(frame_policy_t initial, int maximum_total_kbps,
    bool experimental_packet_control_negotiated, bool experimental_video_pacer_enabled):
      maximum_total_kbps_(maximum_total_kbps),
      experimental_packet_control_negotiated_(experimental_packet_control_negotiated),
      experimental_video_pacer_enabled_(experimental_video_pacer_enabled) {
    if (experimental_packet_control_negotiated && !experimental_video_pacer_enabled)
      throw std::invalid_argument("Packet control requires experimental pacing");
    if (!initial.connection_epoch || initial.revision != 1 || initial.control_epoch != 1 ||
        initial.control_source != control_source_e::legacy || initial.encoder_ceiling_kbps ||
        (initial.automatic_control && (!experimental_packet_control_negotiated || initial.automatic_control->fec ||
                                        initial.automatic_control->activation_epoch ||
                                        initial.automatic_control->maximum_total_kbps != initial.budget.total_kbps)) ||
        initial.encoder_kbps <= 0 || initial.encoder_kbps > 800000 || initial.budget.total_kbps <= 0 ||
        initial.budget.total_kbps > 800000 || maximum_total_kbps < 0 ||
        (maximum_total_kbps && initial.budget.total_kbps > maximum_total_kbps) ||
        initial.fec_base > 100 || initial.fec_key > 100 || initial.fec_recovery > 100) {
      throw std::invalid_argument("Invalid initial transport policy");
    }
    accepted_ = applied_ = std::make_shared<const frame_policy_t>(std::move(initial));
    receipts_.emplace(1, policy_receipt_t { applied_, false, std::nullopt, policy_failure_e::none });
  }

  policy_request_result_t
  policy_state_t::request_normalized(const budget_request_t &budget,
    unsigned fec_base, unsigned fec_key, unsigned fec_recovery,
    std::uint64_t expected_revision, std::uint64_t expected_control_epoch, std::string request_id) {
    std::lock_guard lock(mutex_);
    return request_normalized_locked(budget, fec_base, fec_key, fec_recovery, expected_revision,
      expected_control_epoch, std::move(request_id), control_source_e::manual, true);
  }

  policy_request_result_t
  policy_state_t::request_automatic_control(bool bitrate, bool fec, int maximum_total_kbps,
    std::uint64_t expected_revision, std::uint64_t expected_control_epoch, std::string request_id) {
    std::lock_guard lock(mutex_);
    if (stopped_) return { policy_request_result_e::stopped, {} };
    if (expected_control_epoch != accepted_->control_epoch || expected_revision != accepted_->revision)
      return { policy_request_result_e::conflict, {} };
    if (!experimental_packet_control_negotiated_ || !experimental_video_pacer_enabled_ ||
        accepted_->basis != budget_basis_e::normalized || fec || request_id.empty() || maximum_total_kbps <= 0 ||
        maximum_total_kbps > 800000 || (maximum_total_kbps_ && maximum_total_kbps > maximum_total_kbps_) ||
        accepted_->control_epoch == std::numeric_limits<std::uint64_t>::max()) return {};
    auto budget = accepted_->budget;
    // Increasing an automatic ceiling is permission to estimate, not evidence
    // of capacity. Fixed mode explicitly selects the user's requested budget.
    budget.total_kbps = bitrate ? std::min(budget.total_kbps, maximum_total_kbps) : maximum_total_kbps;
    const automatic_control_t modes { bitrate, fec, maximum_total_kbps, accepted_->control_epoch + 1 };
    return request_normalized_locked(budget, accepted_->fec_base, accepted_->fec_key, accepted_->fec_recovery,
      expected_revision, expected_control_epoch, std::move(request_id), control_source_e::manual, true, {}, modes, true);
  }

  policy_request_result_t
  policy_state_t::request_controller_update(const control_lease_t &lease,
    const budget_request_t &budget, unsigned fec_base, unsigned fec_key, unsigned fec_recovery,
    std::uint64_t expected_revision, std::string request_id, std::optional<int> encoder_ceiling_kbps) {
    std::lock_guard lock(mutex_);
    if (stopped_) return { policy_request_result_e::stopped, {} };
    if (lease.source != control_source_e::googcc && lease.source != control_source_e::local) return {};
    if (lease.connection_epoch != accepted_->connection_epoch || lease.control_epoch != accepted_->control_epoch ||
        lease.source != accepted_->control_source) return { policy_request_result_e::conflict, {} };
    if (accepted_->automatic_control) {
      const auto &modes = *accepted_->automatic_control;
      if (budget.total_kbps > modes.maximum_total_kbps ||
          (!modes.bitrate && budget.total_kbps != accepted_->budget.total_kbps) ||
          (!modes.fec && (fec_base != accepted_->fec_base || fec_key != accepted_->fec_key || fec_recovery != accepted_->fec_recovery)))
        return {};
    }
    return request_normalized_locked(budget, fec_base, fec_key, fec_recovery, expected_revision,
      lease.control_epoch, std::move(request_id), lease.source, false, encoder_ceiling_kbps, accepted_->automatic_control);
  }

  policy_request_result_t
  policy_state_t::prepare_normalized_controller(const control_lease_t &lease,
    const budget_request_t &budget, unsigned fec_base, unsigned fec_key, unsigned fec_recovery,
    std::uint64_t expected_revision) {
    std::lock_guard lock(mutex_);
    if (stopped_) return { policy_request_result_e::stopped, {} };
    if (lease.connection_epoch != accepted_->connection_epoch || lease.control_epoch != accepted_->control_epoch ||
        lease.source != control_source_e::legacy || accepted_->control_source != control_source_e::legacy ||
        accepted_->basis != budget_basis_e::legacy || expected_revision != accepted_->revision) {
      return { policy_request_result_e::conflict, {} };
    }
    if (budget.total_kbps > accepted_->budget.total_kbps) return {};
    return request_normalized_locked(budget, fec_base, fec_key, fec_recovery,
      expected_revision, lease.control_epoch, {}, control_source_e::legacy, false, {}, accepted_->automatic_control);
  }

  policy_request_result_t
  policy_state_t::request_normalized_locked(const budget_request_t &budget,
    unsigned fec_base, unsigned fec_key, unsigned fec_recovery,
    std::uint64_t expected_revision, std::uint64_t expected_control_epoch,
    std::string request_id, control_source_e actor, bool manual_takeover, std::optional<int> encoder_ceiling_kbps,
    std::optional<automatic_control_t> automatic_control, bool force_epoch) {
    if (stopped_) {
      return { policy_request_result_e::stopped, {} };
    }
    // Ownership precedes idempotency: cached success from a previous control
    // generation must not authorize a delayed controller or manual retry.
    if (expected_control_epoch != accepted_->control_epoch ||
        (actor != accepted_->control_source && !manual_takeover)) {
      return { policy_request_result_e::conflict, {} };
    }
    if (request_id.size() > 128 || (encoder_ceiling_kbps && (*encoder_ceiling_kbps <= 0 || *encoder_ceiling_kbps > 800000))) return {};
    if (!request_id.empty()) {
      const auto old = std::find_if(requests_.begin(), requests_.end(), [&](const auto &r) { return r.id == request_id; });
      if (old != requests_.end()) {
        if (old->budget != budget || old->fec_base != fec_base || old->fec_key != fec_key || old->fec_recovery != fec_recovery ||
            old->expected_revision != expected_revision || old->expected_control_epoch != expected_control_epoch || old->actor != actor ||
            old->encoder_ceiling_kbps != encoder_ceiling_kbps || old->automatic_control != automatic_control || old->force_epoch != force_epoch) {
          return { policy_request_result_e::conflict, {} };
        }
        return { policy_request_result_e::accepted, old->policy };
      }
    }
    if (expected_revision != accepted_->revision || expected_control_epoch != accepted_->control_epoch) {
      return { policy_request_result_e::conflict, {} };
    }
    if (fec_base > 100 || fec_key > 100 || fec_recovery > 100 || budget.total_kbps <= 0 ||
        budget.total_kbps > 800000 || (maximum_total_kbps_ && budget.total_kbps > maximum_total_kbps_)) {
      return {};
    }
    auto normalized = budget;
    normalized.fec_numerator = std::max({ fec_base, fec_key, fec_recovery });
    normalized.fec_denominator = 100;
    const auto allocation = allocate_budget(normalized);
    if (!allocation || allocation->encoder_kbps <= 0) {
      return {};
    }
    auto policy = *accepted_;
    if (actor != policy.control_source || force_epoch) {
      if (policy.control_epoch == std::numeric_limits<std::uint64_t>::max()) return {};
      ++policy.control_epoch;
      policy.control_source = actor;
    }
    policy.basis = budget_basis_e::normalized;
    policy.budget = normalized;
    policy.encoder_kbps = allocation->encoder_kbps;
    policy.encoder_ceiling_kbps = encoder_ceiling_kbps;
    policy.automatic_control = automatic_control;
    if (encoder_ceiling_kbps) policy.encoder_kbps = std::min(policy.encoder_kbps, *encoder_ceiling_kbps);
    policy.fec_base = fec_base;
    policy.fec_key = fec_key;
    policy.fec_recovery = fec_recovery;
    // Allocate idempotency storage before publishing acceptance.
    if (!request_id.empty()) requests_.push_back({ std::move(request_id), budget, fec_base, fec_key, fec_recovery,
      expected_revision, expected_control_epoch, actor, {}, encoder_ceiling_kbps, automatic_control, force_epoch });
    policy_request_result_t result;
    try {
      result = accept_locked(std::move(policy));
    }
    catch (...) {
      if (!requests_.empty() && !requests_.back().policy) requests_.pop_back();
      throw;
    }
    if (!requests_.empty() && !requests_.back().policy) {
      if (result.policy)
        requests_.back().policy = result.policy;
      else
        requests_.pop_back();
    }
    if (requests_.size() > 128) requests_.pop_front();
    return result;
  }

  policy_request_result_t
  policy_state_t::transfer_control(const control_lease_t &lease,
    control_source_e target, std::uint64_t expected_revision) {
    std::lock_guard lock(mutex_);
    if (stopped_) return { policy_request_result_e::stopped, {} };
    if (target != control_source_e::manual && target != control_source_e::googcc && target != control_source_e::local) return {};
    if (lease.connection_epoch != accepted_->connection_epoch || lease.control_epoch != accepted_->control_epoch ||
        lease.source != accepted_->control_source || expected_revision != accepted_->revision) {
      return { policy_request_result_e::conflict, {} };
    }
    // An old unversioned ABR cannot safely reacquire a normalized session:
    // return to legacy requires a new connection. Handoff retains the exact
    // normalized budget/FEC, so taking control alone cannot raise the load.
    if (accepted_->basis != budget_basis_e::normalized ||
        accepted_->control_epoch == std::numeric_limits<std::uint64_t>::max()) return {};
    auto policy = *accepted_;
    ++policy.control_epoch;
    policy.control_source = target;
    return accept_locked(std::move(policy));
  }

  policy_request_result_t
  policy_state_t::accept_locked(frame_policy_t policy) {
    if (accepted_->revision == std::numeric_limits<std::uint64_t>::max()) {
      return {};
    }
    policy.revision = accepted_->revision + 1;
    auto immutable = std::make_shared<const frame_policy_t>(std::move(policy));
    // Allocate before changing the published desired state.
    receipts_.emplace(immutable->revision, policy_receipt_t { immutable, false, std::nullopt, policy_failure_e::none });
    auto &previous = receipts_.at(accepted_->revision);
    if (!previous.encoder_applied && accepted_ != applied_ && accepted_ != applying_ &&
        previous.failure == policy_failure_e::none) {
      previous.failure = policy_failure_e::superseded;
    }
    accepted_ = immutable;
    trim_locked();
    return { policy_request_result_e::accepted, std::move(immutable) };
  }

  policy_request_result_t
  policy_state_t::request_legacy_change(std::optional<int> total_kbps,
    std::optional<unsigned> fec_percentage) {
    std::lock_guard lock(mutex_);
    if (stopped_) return { policy_request_result_e::stopped, {} };
    if (accepted_->control_source != control_source_e::legacy || accepted_->basis != budget_basis_e::legacy)
      return { policy_request_result_e::conflict, {} };
    if (!total_kbps && !fec_percentage) return {};
    const int total = total_kbps.value_or(accepted_->budget.total_kbps);
    const unsigned fec = fec_percentage.value_or(accepted_->fec_base);
    if (total <= 0 || total > 800000 || fec > 100 || (maximum_total_kbps_ && total > maximum_total_kbps_)) return {};
    // Preserve the old API's total-bitrate semantics explicitly. New policies
    // use allocate_budget(), never this compatibility discount.
    const int encoder = fec && fec <= 80 ? static_cast<int>(static_cast<std::int64_t>(total) * (100 - fec) / 100) : total;
    if (!encoder) return {};
    auto policy = *accepted_;
    policy.budget.total_kbps = total;
    policy.encoder_kbps = encoder;
    policy.fec_base = policy.fec_key = policy.fec_recovery = fec;
    return accept_locked(std::move(policy));
  }

  frame_policy_ref_t
  policy_state_t::begin_encoder_initialization() {
    std::lock_guard lock(mutex_);
    if (stopped_ || initializing_ || applying_) return {};
    encoder_initialized_ = false;
    initializing_ = applied_;
    return initializing_;
  }

  frame_policy_ref_t
  policy_state_t::active() const {
    std::lock_guard lock(mutex_);
    return applied_;
  }

  frame_policy_ref_t
  policy_state_t::acquire_pending() {
    std::lock_guard lock(mutex_);
    if (stopped_ || !encoder_initialized_ || accepted_ == applied_ || applying_ ||
        receipts_.at(accepted_->revision).failure != policy_failure_e::none) {
      return {};
    }
    applying_ = accepted_;
    return applying_;
  }

  bool
  policy_state_t::acknowledge_encoder(const frame_policy_ref_t &policy, policy_failure_e failure) {
    std::lock_guard lock(mutex_);
    if (stopped_ || !policy || (failure != policy_failure_e::none && failure != policy_failure_e::unsupported && failure != policy_failure_e::backend_failure)) {
      return false;
    }
    const auto iterator = receipts_.find(policy->revision);
    if (iterator == receipts_.end() || iterator->second.policy != policy ||
        (policy != applying_ && policy != initializing_)) {
      return false;
    }
    auto &receipt = iterator->second;
    if (failure == policy_failure_e::none) {
      receipt.encoder_applied = true;
      receipt.failure = policy_failure_e::none;
      applied_ = policy;
      encoder_initialized_ = true;
    }
    else {
      // A failed later initialization does not erase a historical successful
      // application. Readiness is separate from the immutable policy receipt.
      if (!receipt.encoder_applied) receipt.failure = failure;
      if (failure == policy_failure_e::backend_failure) encoder_initialized_ = false;
    }
    if (applying_ == policy) {
      applying_.reset();
    }
    if (initializing_ == policy) initializing_.reset();
    trim_locked();
    return true;
  }

  bool
  policy_state_t::acknowledge_first_sent(const frame_policy_ref_t &policy, std::uint64_t frame) {
    std::lock_guard lock(mutex_);
    if (stopped_ || !policy) {
      return false;
    }
    const auto iterator = receipts_.find(policy->revision);
    if (iterator == receipts_.end() || iterator->second.policy != policy || !iterator->second.encoder_applied ||
        iterator->second.failure != policy_failure_e::none) {
      return false;
    }
    if (!iterator->second.first_sent_frame) {
      iterator->second.first_sent_frame = frame;
    }
    return true;
  }

  policy_snapshot_t
  policy_state_t::snapshot() const {
    std::lock_guard lock(mutex_);
    policy_snapshot_t result { accepted_, applied_, encoder_initialized_, stopped_, {} };
    result.experimental_packet_control_negotiated = experimental_packet_control_negotiated_;
    result.experimental_video_pacer_enabled = experimental_video_pacer_enabled_;
    result.receipts.reserve(receipts_.size());
    for (const auto &[revision, receipt] : receipts_) {
      result.receipts.push_back(receipt);
    }
    return result;
  }

  bool
  policy_state_t::stopped() const {
    std::lock_guard lock(mutex_);
    return stopped_;
  }

  void
  policy_state_t::stop() {
    std::lock_guard lock(mutex_);
    stopped_ = true;
    for (auto &[revision, receipt] : receipts_) {
      if (!receipt.encoder_applied && receipt.failure == policy_failure_e::none) {
        receipt.failure = policy_failure_e::stopped;
      }
    }
    applying_.reset();
    initializing_.reset();
  }

  void
  policy_state_t::trim_locked() {
    while (receipts_.size() > receipt_capacity) {
      auto iterator = std::find_if(receipts_.begin(), receipts_.end(), [&](const auto &item) {
        return item.second.policy != accepted_ && item.second.policy != applied_ && item.second.policy != applying_ &&
               item.second.policy != initializing_;
      });
      receipts_.erase(iterator);
    }
  }

  void
  frame_policy_history_t::bind(std::uint64_t frame, frame_policy_ref_t policy) {
    entries_[frame % capacity] = { frame, std::move(policy) };
  }

  frame_policy_ref_t
  frame_policy_history_t::find(std::uint64_t frame) const {
    const auto &entry = entries_[frame % capacity];
    return entry.frame == frame ? entry.policy : frame_policy_ref_t {};
  }
}  // namespace transport
