#pragma once

#include "googcc_adapter.h"
#include "transport_fec_controller.h"
#include "transport_policy.h"

namespace transport {
  struct googcc_runtime_config_t {
    googcc_config_t controller;
    bool feedback_negotiated = false;
    bool control_negotiated = false;
    bool deadline_pacing_enabled = false;
    std::int64_t feedback_timeout_us = 1000000;
    std::int64_t policy_interval_us = 250000;
    bool automatic_bitrate_enabled = true;
    bool automatic_fec_enabled = false;
    bool budgeted_probing_enabled = false;
    fec_selection_config_t fec;
  };

  struct googcc_runtime_snapshot_t {
    googcc_snapshot_t estimate;
    std::optional<control_lease_t> lease;
    std::uint64_t accepted_policy_requests = 0;
    std::uint64_t rejected_requests = 0;
    std::uint64_t rejected_probe_requests = 0;
    bool control_revoked = false;
    bool feedback_stale = true;
    std::uint64_t accepted_fec_requests = 0;
    std::uint64_t infeasible_fec_decisions = 0;
    fec_selection_t fec_selection;
    std::int64_t fec_replay_at_us = -1;
    std::int64_t fec_replay_duration_us = 0;
    fec_selection_context_t fec_selection_context;
  };

  // The video owner serializes OS receipts, authenticated feedback and timers.
  // This class never maintains another packet ledger or transmits probes.
  // Observer creation cannot grant control. A verified explicit negotiation,
  // applied normalized startup/manual policy, actual first send and new covered feedback are
  // required for a one-time handoff. Manual preemption is irreversible here.
  class googcc_runtime_t {
  public:
    googcc_runtime_t(googcc_runtime_config_t config, std::shared_ptr<policy_state_t> policy,
      frame_policy_ref_t activation_policy = {});
    bool
    on_successful_send(const successful_send_event_t &event);
    bool
    on_feedback(const feedback_event_t &event);
    bool
    try_take_control(std::int64_t now_us, std::optional<googcc_queue_sample_t> queue = {});
    bool
    process_interval(std::int64_t now_us, const protection_trace_t *protection = nullptr,
      std::optional<googcc_queue_sample_t> queue = {});
    bool
    needs_protection_trace(std::int64_t now_us) const;
    bool
    probe_eligible(std::int64_t now_us) const;
    std::vector<googcc_probe_t>
    take_probe_requests(std::int64_t now_us);
    void
    stop() noexcept;
    googcc_runtime_snapshot_t
    snapshot() const;

  private:
    bool
    fresh(std::int64_t now_us) const;
    bool
    retain_lease(const frame_policy_ref_t &accepted);
    void
    collect_probes();
    const googcc_runtime_config_t config_;
    const std::shared_ptr<policy_state_t> policy_;
    frame_policy_ref_t activation_policy_;
    googcc_adapter_t controller_;
    std::optional<control_lease_t> lease_;
    bool granted_ = false;
    bool revoked_ = false;
    bool stale_ = true;
    std::int64_t last_policy_us_ = -1;
    std::uint64_t accepted_policy_requests_ = 0;
    std::uint64_t rejected_requests_ = 0;
    std::uint64_t rejected_probes_ = 0;
    std::vector<googcc_probe_t> pending_probes_;
    std::uint64_t accepted_fec_requests_ = 0;
    std::uint64_t infeasible_fec_decisions_ = 0;
    fec_selection_t fec_selection_;
    std::int64_t fec_replay_at_us_ = -1;
    std::int64_t fec_replay_duration_us_ = 0;
    fec_selection_context_t fec_selection_context_;
    std::uint64_t protection_clock_epoch_ = 0;
    std::int64_t clean_start_us_ = -1;
    std::int64_t clean_end_us_ = -1;
    std::optional<std::uint64_t> clean_through_ordinal_;
    std::int64_t last_fec_change_us_ = -1;
  };
}  // namespace transport
