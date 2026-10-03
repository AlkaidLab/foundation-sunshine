#include "src/transport/googcc_runtime.h"
#include "src/transport/transport_feedback_wire.h"

#include <array>
#include <stdexcept>

#include <gtest/gtest.h>

namespace {
  using namespace transport;

  struct runtime_fixture_t {
    runtime_fixture_t() {
      frame_policy_t initial;
      initial.connection_epoch = 7;
      initial.budget.total_kbps = 30000;
      initial.encoder_kbps = 27000;
      initial.fec_base = initial.fec_key = initial.fec_recovery = 10;
      policy = std::make_shared<policy_state_t>(initial, 50000, true, true);
      budget.total_kbps = 30000;
      budget.other_kbps = budget.repair_kbps = budget.probe_kbps = 500;
      budget.video_overhead_kbps = 500;
      const auto manual = policy->request_normalized(budget, 10, 30, 20, 1, 1);
      EXPECT_EQ(manual.result, policy_request_result_e::accepted);
      config.controller.connection_epoch = 7;
      config.controller.maximum_kbps = config.controller.initial_kbps = 28500;
      config.feedback_negotiated = config.control_negotiated = config.deadline_pacing_enabled = true;
    }

    void
    apply() {
      const auto pending = policy->begin_encoder_initialization();
      ASSERT_TRUE(policy->acknowledge_encoder(pending, policy_failure_e::none));
      const auto normalized = policy->acquire_pending();
      ASSERT_TRUE(normalized);
      ASSERT_TRUE(policy->acknowledge_encoder(normalized, policy_failure_e::none));
      ASSERT_EQ(policy->snapshot().applied, policy->snapshot().accepted);
    }

    void
    feedback(googcc_runtime_t &runtime, std::int64_t at, unsigned lost = 0, bool first_sent = true, unsigned frame_shards = 20,
      std::uint64_t receiver_epoch = 1, std::int64_t feedback_delay_us = 0) {
      TF_PACKET_REPORT report {};
      report.connectionEpoch = 7;
      report.reportSequence = ++report_sequence;
      report.receiverClockEpoch = receiver_epoch;
      report.receiverSampleTimeUs = 9000000 + at + 10000;
      report.baseExtendedSequence = next_sequence;
      report.packetCount = 20;
      const auto accepted = policy->snapshot().accepted;
      for (unsigned i = 0; i < report.packetCount; ++i) {
        sent_packet_t sent { next_sequence++, at + i * 100, 1280, 37, accepted->revision, packet_kind_e::data, {} };
        const auto layout = plan_fec_block(frame_shards, accepted->fec_base, 0);
        ASSERT_TRUE(layout);
        const auto shard = static_cast<std::uint16_t>(i % layout->total_shards());
        sent.kind = shard < frame_shards ? packet_kind_e::data : packet_kind_e::fec;
        sent.protection = { static_cast<std::uint16_t>(frame_shards), layout->total_shards(), shard,
          0, protection_class_e::base, static_cast<std::uint16_t>(frame_shards), 1 };
        const auto event = wire.commit_success_event(sent);
        ASSERT_TRUE(event);
        ASSERT_TRUE(runtime.on_successful_send(*event));
        if (first_sent) policy->acknowledge_first_sent(accepted, 37);
        report.status[i] = i < lost ? TF_MISSING : TF_RECEIVED;
        report.firstArrivalTimeUs[i] = i < lost ? TF_NO_ARRIVAL : 9000000 + at + i * 100 + 5000;
      }
      std::array<std::uint8_t, TF_MAX_REPORT_BYTES> bytes {};
      const auto size = TfEncodeReport(&report, bytes.data(), bytes.size());
      ASSERT_GT(size, 0U);
      const auto event = wire.apply_wire_event(std::span(bytes.data(), size), at + 10000 + feedback_delay_us);
      ASSERT_EQ(event.feedback.result, report_result_e::accepted);
      ASSERT_TRUE(runtime.on_feedback(event));
    }

    std::shared_ptr<policy_state_t> policy;
    budget_request_t budget;
    googcc_runtime_config_t config;
    wire_feedback_t wire { 7, true };
    std::uint64_t report_sequence = 0;
    std::uint64_t next_sequence = 0;
  };

  TEST(GoogCcRuntime, BudgetedProbeRequestsAwaitRealHandoffAndExpireWithoutSending) {
    runtime_fixture_t f;
    f.config.budgeted_probing_enabled = true;
    googcc_runtime_t runtime(f.config, f.policy);
    EXPECT_TRUE(runtime.take_probe_requests(0).empty());
    EXPECT_FALSE(runtime.probe_eligible(0));
    f.apply();
    f.feedback(runtime, 1000);
    ASSERT_TRUE(runtime.try_take_control(11000));
    ASSERT_TRUE(runtime.probe_eligible(11000));
    const auto requests = runtime.take_probe_requests(11000);
    ASSERT_EQ(requests.size(), 2U);
    for (const auto &request : requests) {
      EXPECT_GE(request.cluster_id, 0);
      EXPECT_EQ(request.requested_at_us, 0);
      EXPECT_GT(request.target_kbps, 0);
      EXPECT_GT(request.duration_us, 0);
      EXPECT_GT(request.minimum_delta_us, 0);
      EXPECT_EQ(request.minimum_packets, 5);
    }
    EXPECT_TRUE(runtime.take_probe_requests(11000).empty());
    EXPECT_FALSE(runtime.probe_eligible(1011001));
    EXPECT_EQ(runtime.snapshot().estimate.accepted_sends, 20U);
    // Reading a request cannot manufacture sent packets or feedback.
    EXPECT_EQ(runtime.snapshot().estimate.feedback_packet_changes, 20U);
  }

  TEST(GoogCcRuntime, ProbeAuthorityDisappearsImmediatelyOnManualPreemptionAndStop) {
    runtime_fixture_t f;
    f.config.budgeted_probing_enabled = true;
    googcc_runtime_t runtime(f.config, f.policy);
    f.apply();
    f.feedback(runtime, 1000);
    ASSERT_TRUE(runtime.try_take_control(11000));
    ASSERT_TRUE(runtime.probe_eligible(11000));
    const auto accepted = f.policy->snapshot().accepted;
    ASSERT_EQ(f.policy->request_normalized(f.budget, 10, 30, 20, accepted->revision, accepted->control_epoch).result,
      policy_request_result_e::accepted);
    EXPECT_FALSE(runtime.probe_eligible(11000));
    EXPECT_TRUE(runtime.take_probe_requests(11000).empty());
    EXPECT_EQ(runtime.snapshot().rejected_probe_requests, 2U);
    runtime.stop();
    EXPECT_FALSE(runtime.probe_eligible(11000));
  }

  TEST(GoogCcRuntime, FixedBitrateAndUnreadyHandoffCannotReleaseProbeRequests) {
    for (const bool fixed : { false, true }) {
      runtime_fixture_t f;
      f.config.budgeted_probing_enabled = true;
      f.config.automatic_bitrate_enabled = !fixed;
      googcc_runtime_t runtime(f.config, f.policy);
      EXPECT_TRUE(runtime.take_probe_requests(1000001).empty());
      EXPECT_EQ(runtime.snapshot().rejected_probe_requests, 2U);
      EXPECT_FALSE(runtime.probe_eligible(1000001));
      EXPECT_EQ(runtime.snapshot().estimate.accepted_sends, 0U);
    }
  }

  TEST(GoogCcRuntime, ReceiverClockResetRetiresOldRequestsBeforeNewRouteProbes) {
    runtime_fixture_t f;
    f.config.budgeted_probing_enabled = true;
    googcc_runtime_t runtime(f.config, f.policy);
    f.apply();
    f.feedback(runtime, 1000);
    ASSERT_TRUE(runtime.try_take_control(11000));
    // Leave both initial requests pending, then change the real receiver epoch.
    f.feedback(runtime, 20000, 0, true, 20, 2);
    ASSERT_EQ(runtime.snapshot().estimate.receiver_clock_resets, 1U);
    EXPECT_GE(runtime.snapshot().rejected_probe_requests, 2U);
    const auto requests = runtime.take_probe_requests(30000);
    ASSERT_FALSE(requests.empty());
    for (const auto &request : requests) {
      EXPECT_GE(request.cluster_id, 2);
      EXPECT_EQ(request.requested_at_us, 30000);
    }
    EXPECT_EQ(runtime.snapshot().estimate.accepted_sends, 40U);
  }

  TEST(GoogCcRuntime, RejectsUnnegotiatedFeedbackInvalidTimingAndForeignPolicy) {
    runtime_fixture_t f;
    auto config = f.config;
    config.feedback_negotiated = false;
    EXPECT_THROW(googcc_runtime_t(config, f.policy), std::invalid_argument);
    config = f.config;
    config.feedback_timeout_us = 0;
    EXPECT_THROW(googcc_runtime_t(config, f.policy), std::invalid_argument);
    config = f.config;
    config.policy_interval_us = -1;
    EXPECT_THROW(googcc_runtime_t(config, f.policy), std::invalid_argument);
    config = f.config;
    config.controller.connection_epoch = 8;
    EXPECT_THROW(googcc_runtime_t(config, f.policy), std::invalid_argument);
    EXPECT_THROW(googcc_runtime_t(f.config, {}), std::invalid_argument);
  }

  TEST(GoogCcRuntime, ActualObserverEventsNeverGrantOrModifyPolicy) {
    runtime_fixture_t f;
    f.config.control_negotiated = false;
    googcc_runtime_t runtime(f.config, f.policy);
    const auto accepted = f.policy->snapshot().accepted;
    f.apply();
    f.feedback(runtime, 1000);
    EXPECT_FALSE(runtime.try_take_control(11000));
    ASSERT_TRUE(runtime.process_interval(20000));
    const auto snapshot = runtime.snapshot();
    EXPECT_EQ(snapshot.estimate.accepted_sends, 20U);
    EXPECT_EQ(snapshot.estimate.feedback_batches, 1U);
    EXPECT_EQ(snapshot.estimate.feedback_packet_changes, 20U);
    EXPECT_GT(snapshot.rejected_probe_requests, 0U);
    EXPECT_FALSE(snapshot.lease);
    EXPECT_EQ(snapshot.accepted_policy_requests, 0U);
    EXPECT_EQ(f.policy->snapshot().accepted, accepted);
  }

  TEST(GoogCcRuntime, QueuePressureCapsEncoderAndPreservesDrainBudgetThroughPendingAndRecovery) {
    runtime_fixture_t f;
    f.config.controller.pacer_queue_feedback = f.config.controller.queue_pushback = true;
    f.config.automatic_bitrate_enabled = false;
    googcc_runtime_t runtime(f.config, f.policy);
    f.apply();
    f.feedback(runtime, 1000);
    ASSERT_TRUE(runtime.try_take_control(11000, googcc_queue_sample_t { 7, 11000, 0, true, false }));
    f.apply();
    const auto original = f.policy->snapshot().accepted;
    ASSERT_TRUE(runtime.process_interval(300000, nullptr, googcc_queue_sample_t { 7, 300000, 2000000, true, false }));
    const auto reduced = f.policy->snapshot().accepted;
    ASSERT_TRUE(reduced->encoder_ceiling_kbps);
    EXPECT_LT(reduced->encoder_kbps, original->encoder_kbps);
    EXPECT_EQ(reduced->budget, original->budget);
    EXPECT_EQ(f.policy->snapshot().applied, original);
    EXPECT_FALSE(f.policy->snapshot().receipts.back().encoder_applied);
    EXPECT_FALSE(f.policy->snapshot().receipts.back().first_sent_frame);
    ASSERT_TRUE(runtime.process_interval(550000, nullptr, googcc_queue_sample_t { 7, 550000, 0, true, false }));
    EXPECT_EQ(f.policy->snapshot().accepted, reduced);
    f.apply();
    f.feedback(runtime, 600000);
    ASSERT_TRUE(runtime.process_interval(800000, nullptr, googcc_queue_sample_t { 7, 800000, 0, true, false }));
    const auto recovered = f.policy->snapshot().accepted;
    EXPECT_FALSE(recovered->encoder_ceiling_kbps);
    EXPECT_EQ(recovered->encoder_kbps, original->encoder_kbps);
    EXPECT_EQ(recovered->budget, original->budget);
    EXPECT_EQ(f.policy->snapshot().applied, reduced);
  }

  TEST(GoogCcRuntime, QueuePresenceCannotReplaceNegotiationApplicationOrManualOwnership) {
    runtime_fixture_t f;
    f.config.controller.pacer_queue_feedback = f.config.controller.queue_pushback = true;
    googcc_runtime_t runtime(f.config, f.policy);
    f.feedback(runtime, 1000, 0, false);
    const googcc_queue_sample_t first { 7, 11000, 0, true, false };
    EXPECT_FALSE(runtime.try_take_control(11000, first));
    f.apply();
    f.policy->acknowledge_first_sent(f.policy->snapshot().accepted, 37);
    EXPECT_FALSE(runtime.try_take_control(11000));
    EXPECT_FALSE(runtime.try_take_control(11000, googcc_queue_sample_t { 8, 11000, 0, true, false }));
    ASSERT_TRUE(runtime.try_take_control(11000, first));
    const auto current = f.policy->snapshot().accepted;
    const auto manual = f.policy->request_normalized(f.budget, 10, 30, 20, current->revision, current->control_epoch);
    ASSERT_EQ(manual.result, policy_request_result_e::accepted);
    ASSERT_TRUE(runtime.process_interval(300000, nullptr, googcc_queue_sample_t { 7, 300000, 2000000, true, false }));
    EXPECT_EQ(f.policy->snapshot().accepted, manual.policy);
    EXPECT_FALSE(manual.policy->encoder_ceiling_kbps);
    EXPECT_FALSE(runtime.snapshot().lease);
    EXPECT_TRUE(runtime.snapshot().control_revoked);
  }

  TEST(GoogCcRuntime, LeaseRequiresAppliedPolicyFirstActualSendAndFreshCoverage) {
    runtime_fixture_t f;
    googcc_runtime_t runtime(f.config, f.policy);
    EXPECT_FALSE(runtime.try_take_control(1000));
    f.feedback(runtime, 1000, 0, false);
    EXPECT_FALSE(runtime.try_take_control(11000));
    f.apply();
    EXPECT_FALSE(runtime.try_take_control(11000));
    ASSERT_TRUE(f.policy->acknowledge_first_sent(f.policy->snapshot().accepted, 37));
    ASSERT_TRUE(runtime.try_take_control(11000));
    const auto lease = runtime.snapshot().lease;
    ASSERT_TRUE(lease);
    EXPECT_EQ(lease->source, control_source_e::googcc);
    EXPECT_EQ(lease->control_epoch, 3U);
    EXPECT_EQ(f.policy->snapshot().accepted->budget.total_kbps, 30000);
    EXPECT_EQ(f.policy->snapshot().accepted->fec_key, 30U);
    EXPECT_FALSE(runtime.try_take_control(12000));
  }

  TEST(GoogCcRuntime, DeadlinePacingGateAndFeedbackAgePreventGrant) {
    runtime_fixture_t f;
    f.config.deadline_pacing_enabled = false;
    googcc_runtime_t runtime(f.config, f.policy);
    f.apply();
    f.feedback(runtime, 1000);
    EXPECT_FALSE(runtime.try_take_control(11000));
    f.config.deadline_pacing_enabled = true;
    googcc_runtime_t other(f.config, f.policy);
    f.feedback(other, 30000);
    EXPECT_FALSE(other.try_take_control(1040001));
    EXPECT_FALSE(other.snapshot().lease);
  }

  TEST(GoogCcRuntime, DelayedNewCoverageCannotGrantUntilCurrentActualPacketsAreCovered) {
    runtime_fixture_t f;
    f.config.budgeted_probing_enabled = true;
    googcc_runtime_t runtime(f.config, f.policy);
    f.apply();
    f.feedback(runtime, 1000, 0, true, 20, 1, 1200000);
    EXPECT_EQ(runtime.snapshot().estimate.last_covered_feedback_us, 1211000);
    EXPECT_EQ(runtime.snapshot().estimate.last_covered_send_us, 2900);
    EXPECT_EQ(f.wire.snapshot().ledger.received_packets, 20U);
    EXPECT_FALSE(runtime.try_take_control(1211000));
    EXPECT_FALSE(runtime.probe_eligible(1211000));
    EXPECT_FALSE(runtime.snapshot().lease);
    f.feedback(runtime, 1220000);
    ASSERT_TRUE(runtime.try_take_control(1230000));
    EXPECT_TRUE(runtime.probe_eligible(1230000));
    EXPECT_EQ(f.wire.snapshot().ledger.received_packets, 40U);
  }

  TEST(GoogCcRuntime, LateCoveragePreservesStatisticsButCannotRefreshProbeOrFecAuthority) {
    runtime_fixture_t f;
    f.config.budgeted_probing_enabled = f.config.automatic_fec_enabled = true;
    googcc_runtime_t runtime(f.config, f.policy);
    f.apply();
    f.feedback(runtime, 1000);
    ASSERT_TRUE(runtime.try_take_control(11000));
    EXPECT_FALSE(runtime.take_probe_requests(11000).empty());
    f.apply();
    const auto before = f.policy->snapshot().accepted;
    f.feedback(runtime, 20000, 0, true, 20, 1, 1200000);
    EXPECT_EQ(runtime.snapshot().estimate.last_covered_feedback_us, 1230000);
    EXPECT_EQ(runtime.snapshot().estimate.last_covered_send_us, 21900);
    EXPECT_FALSE(runtime.probe_eligible(1230000));
    EXPECT_FALSE(runtime.needs_protection_trace(1230000));
    ASSERT_TRUE(runtime.process_interval(1230000));
    EXPECT_TRUE(runtime.snapshot().feedback_stale);
    EXPECT_TRUE(runtime.take_probe_requests(1230000).empty());
    const auto after = f.policy->snapshot().accepted;
    EXPECT_LE(after->budget.total_kbps, before->budget.total_kbps);
    EXPECT_EQ(after->fec_base, before->fec_base);
    EXPECT_EQ(after->fec_key, before->fec_key);
    EXPECT_EQ(after->fec_recovery, before->fec_recovery);
    EXPECT_EQ(runtime.snapshot().estimate.feedback_packet_changes, 40U);
    EXPECT_EQ(f.wire.snapshot().ledger.received_packets, 40U);
    if (f.policy->snapshot().applied != f.policy->snapshot().accepted) f.apply();
    f.feedback(runtime, 1250000);
    ASSERT_TRUE(runtime.process_interval(1260000));
    EXPECT_FALSE(runtime.snapshot().feedback_stale);
    EXPECT_TRUE(runtime.probe_eligible(1260000));
    EXPECT_EQ(f.wire.snapshot().ledger.received_packets, 60U);
  }

  TEST(GoogCcRuntime, CoverageExpiryUsesActualSendBoundaryWithoutRequiringNewOutput) {
    runtime_fixture_t f;
    f.config.budgeted_probing_enabled = true;
    googcc_runtime_t runtime(f.config, f.policy);
    f.apply();
    f.feedback(runtime, 1000);
    ASSERT_TRUE(runtime.try_take_control(11000));
    EXPECT_TRUE(runtime.probe_eligible(1002900));
    EXPECT_FALSE(runtime.probe_eligible(1002901));
    ASSERT_TRUE(runtime.process_interval(1002901));
    EXPECT_TRUE(runtime.snapshot().feedback_stale);
    EXPECT_EQ(runtime.snapshot().estimate.accepted_sends, 20U);
    EXPECT_EQ(runtime.snapshot().estimate.feedback_packet_changes, 20U);
  }

  TEST(GoogCcRuntime, ManualChangeBeforeGrantInvalidatesTheOriginalActivation) {
    runtime_fixture_t f;
    googcc_runtime_t runtime(f.config, f.policy);
    f.apply();
    f.feedback(runtime, 1000);
    const auto current = f.policy->snapshot().accepted;
    ASSERT_EQ(f.policy->request_normalized(f.budget, 10, 30, 20, current->revision, current->control_epoch).result,
      policy_request_result_e::accepted);
    EXPECT_FALSE(runtime.try_take_control(11000));
    EXPECT_TRUE(runtime.snapshot().control_revoked);
  }

  TEST(GoogCcRuntime, ManualPreemptionAndControllerReplacementCannotReacquire) {
    for (const bool manual : { false, true }) {
      runtime_fixture_t f;
      googcc_runtime_t runtime(f.config, f.policy);
      f.apply();
      f.feedback(runtime, 1000);
      ASSERT_TRUE(runtime.try_take_control(11000));
      const auto current = f.policy->snapshot().accepted;
      if (manual) {
        ASSERT_EQ(f.policy->request_normalized(f.budget, 10, 30, 20, current->revision, current->control_epoch).result,
          policy_request_result_e::accepted);
      }
      else {
        ASSERT_EQ(f.policy->transfer_control(*runtime.snapshot().lease, control_source_e::googcc, current->revision).result,
          policy_request_result_e::accepted);
      }
      const auto replacement = f.policy->snapshot().accepted;
      ASSERT_TRUE(runtime.process_interval(300000));
      EXPECT_FALSE(runtime.snapshot().lease);
      EXPECT_TRUE(runtime.snapshot().control_revoked);
      EXPECT_FALSE(runtime.try_take_control(300000));
      EXPECT_EQ(f.policy->snapshot().accepted, replacement);
    }
  }

  TEST(GoogCcRuntime, RealLossEstimationChangesAcceptedBudgetWithinTheUserCeiling) {
    runtime_fixture_t f;
    googcc_runtime_t runtime(f.config, f.policy);
    f.apply();
    f.feedback(runtime, 1000);
    ASSERT_TRUE(runtime.try_take_control(11000));
    const auto applied = f.policy->snapshot().applied;
    for (int i = 0; i < 100; ++i) {
      const auto at = 30000 + i * 50000;
      f.feedback(runtime, at, 6);
      ASSERT_TRUE(runtime.process_interval(at + 10000));
      const auto accepted = f.policy->snapshot().accepted;
      EXPECT_LE(accepted->budget.total_kbps, 30000);
      EXPECT_EQ(accepted->budget.other_kbps, 500);
      EXPECT_EQ(accepted->budget.repair_kbps, 500);
      EXPECT_EQ(accepted->budget.probe_kbps, 500);
      EXPECT_EQ(accepted->budget.video_overhead_kbps, 500);
      EXPECT_EQ(accepted->fec_key, 30U);
    }
    EXPECT_GT(runtime.snapshot().accepted_policy_requests, 0U);
    EXPECT_LT(f.policy->snapshot().accepted->budget.total_kbps, 30000);
    EXPECT_EQ(f.policy->snapshot().applied, applied);
    EXPECT_EQ(runtime.snapshot().estimate.rejected_send_events, 0U);
    EXPECT_EQ(runtime.snapshot().estimate.rejected_feedback_events, 0U);
    const auto before = f.policy->snapshot().accepted->budget.total_kbps;
    ASSERT_TRUE(runtime.process_interval(7000000));
    EXPECT_TRUE(runtime.snapshot().feedback_stale);
    EXPECT_LE(f.policy->snapshot().accepted->budget.total_kbps, before);
  }

  TEST(GoogCcRuntime, StoppedPolicyCannotGrantOrKeepTheLease) {
    runtime_fixture_t f;
    googcc_runtime_t runtime(f.config, f.policy);
    f.apply();
    f.feedback(runtime, 1000);
    ASSERT_TRUE(runtime.try_take_control(11000));
    f.policy->stop();
    ASSERT_TRUE(runtime.process_interval(20000));
    EXPECT_FALSE(runtime.snapshot().lease);
    EXPECT_TRUE(runtime.snapshot().control_revoked);
    EXPECT_FALSE(runtime.try_take_control(20000));
  }

  TEST(GoogCcRuntime, TimerCannotRewriteHistoricalSubmissionTime) {
    runtime_fixture_t f;
    googcc_runtime_t runtime(f.config, f.policy);
    ASSERT_TRUE(runtime.process_interval(1000));
    const auto event = f.wire.commit_success_event({ 0, 999, 1280, 1, 2, packet_kind_e::data, {} });
    ASSERT_TRUE(event);
    EXPECT_FALSE(runtime.on_successful_send(*event));
    EXPECT_EQ(runtime.snapshot().estimate.rejected_send_events, 1U);
    EXPECT_FALSE(runtime.process_interval(999));
    EXPECT_EQ(f.wire.snapshot().ledger.committed_packets, 1U);
  }
  TEST(GoogCcRuntime, NormalizedStartupCanGrantAfterActualApplySendAndFeedback) {
    runtime_fixture_t f;
    frame_policy_t initial;
    initial.connection_epoch = 7;
    initial.budget.total_kbps = 30000;
    initial.encoder_kbps = 27000;
    f.policy = std::make_shared<policy_state_t>(initial, 50000, true, true);
    ASSERT_EQ(f.policy->prepare_normalized_controller({ 7, 1, control_source_e::legacy }, f.budget, 10, 30, 20, 1).result,
      policy_request_result_e::accepted);
    googcc_runtime_t runtime(f.config, f.policy);
    EXPECT_FALSE(runtime.try_take_control(1000));
    f.apply();
    f.feedback(runtime, 1000);
    ASSERT_TRUE(runtime.try_take_control(11000));
    EXPECT_EQ(runtime.snapshot().lease->control_epoch, 2U);
    EXPECT_FALSE(runtime.snapshot().feedback_stale);
    EXPECT_EQ(f.policy->snapshot().accepted->control_source, control_source_e::googcc);
    EXPECT_EQ(f.policy->snapshot().accepted->budget.total_kbps, 30000);
  }

  TEST(GoogCcRuntime, BareLegacyCannotGrantEvenWithAppliedSendAndCoverage) {
    runtime_fixture_t f;
    frame_policy_t initial;
    initial.connection_epoch = 7;
    initial.budget.total_kbps = 30000;
    initial.encoder_kbps = 27000;
    f.policy = std::make_shared<policy_state_t>(initial, 50000);
    googcc_runtime_t runtime(f.config, f.policy);
    ASSERT_TRUE(f.policy->acknowledge_encoder(f.policy->begin_encoder_initialization(), policy_failure_e::none));
    f.feedback(runtime, 1000);
    EXPECT_FALSE(runtime.try_take_control(11000));
    EXPECT_EQ(f.policy->snapshot().accepted->control_source, control_source_e::legacy);
  }

  TEST(GoogCcRuntime, BackendNotReadyCannotLicenseAnAutomaticUpstep) {
    runtime_fixture_t f;
    googcc_runtime_t runtime(f.config, f.policy);
    f.apply();
    f.feedback(runtime, 1000);
    ASSERT_TRUE(runtime.try_take_control(11000));
    auto smaller = f.budget;
    smaller.total_kbps = 8000;
    ASSERT_EQ(f.policy->request_controller_update(*runtime.snapshot().lease, smaller, 10, 30, 20,
                        f.policy->snapshot().accepted->revision)
                .result,
      policy_request_result_e::accepted);
    ASSERT_TRUE(f.policy->acknowledge_encoder(f.policy->acquire_pending(), policy_failure_e::backend_failure));
    ASSERT_FALSE(f.policy->snapshot().encoder_initialized);
    ASSERT_TRUE(runtime.process_interval(300000));
    EXPECT_LE(f.policy->snapshot().accepted->budget.total_kbps, 8000);
  }
  TEST(GoogCcRuntime, FlowCloseExplicitlyRevokesBeforeFinalTraceAndPreventsRegrant) {
    runtime_fixture_t f;
    googcc_runtime_t runtime(f.config, f.policy);
    f.apply();
    f.feedback(runtime, 1000);
    ASSERT_TRUE(runtime.try_take_control(11000));
    runtime.stop();
    EXPECT_FALSE(runtime.snapshot().lease);
    EXPECT_TRUE(runtime.snapshot().control_revoked);
    EXPECT_FALSE(runtime.try_take_control(11000));
    ASSERT_TRUE(runtime.process_interval(300000));
    EXPECT_EQ(runtime.snapshot().accepted_policy_requests, 0U);
  }

  TEST(GoogCcRuntime, AutomaticFecUsesAuthoritativeReplayAtFixedBudgetWithoutFakingApplication) {
    runtime_fixture_t f;
    f.config.automatic_bitrate_enabled = false;
    f.config.automatic_fec_enabled = true;
    googcc_runtime_t runtime(f.config, f.policy);
    f.apply();
    f.feedback(runtime, 1000);
    ASSERT_TRUE(runtime.try_take_control(11000));
    EXPECT_FALSE(runtime.needs_protection_trace(12000));
    frame_policy_ref_t applied_before;
    for (int i = 0; i < 100 && !runtime.snapshot().accepted_fec_requests; ++i) {
      const auto pending = f.policy->acquire_pending();
      if (pending) {
        ASSERT_TRUE(f.policy->acknowledge_encoder(pending, policy_failure_e::none));
      }
      const auto at = 30000 + i * 50000;
      f.feedback(runtime, at, 2);
      applied_before = f.policy->snapshot().applied;
      const auto trace = f.wire.protection_trace(at + 10000);
      ASSERT_TRUE(runtime.process_interval(at + 10000, &trace));
    }
    ASSERT_GT(runtime.snapshot().accepted_fec_requests, 0U);
    const auto accepted = f.policy->snapshot().accepted;
    EXPECT_GT(accepted->fec_base, 10U);
    EXPECT_EQ(accepted->budget.total_kbps, 30000);
    EXPECT_EQ(accepted->budget.other_kbps, 500);
    EXPECT_EQ(accepted->budget.repair_kbps, 500);
    EXPECT_EQ(accepted->budget.probe_kbps, 500);
    EXPECT_EQ(f.policy->snapshot().applied, applied_before);
    EXPECT_LE(accepted->encoder_kbps, applied_before->encoder_kbps);
  }

  TEST(GoogCcRuntime, BestEffortCanApplyAtFixedBudgetWhileStrictTargetRemainsUnmet) {
    runtime_fixture_t f;
    const auto before = f.policy->snapshot().accepted;
    ASSERT_EQ(f.policy->request_normalized(f.budget, 0, 0, 0, before->revision, before->control_epoch).result,
      policy_request_result_e::accepted);
    f.config.automatic_bitrate_enabled = false;
    f.config.automatic_fec_enabled = true;
    f.config.fec.minimum_parity = 2;
    googcc_runtime_t runtime(f.config, f.policy);
    f.apply();
    f.feedback(runtime, 1000, 0, true, 1);
    ASSERT_TRUE(runtime.try_take_control(11000));
    auto pending = f.policy->acquire_pending();
    ASSERT_TRUE(pending);
    ASSERT_TRUE(f.policy->acknowledge_encoder(pending, policy_failure_e::none));
    const auto applied = f.policy->snapshot().applied;
    for (int i = 0; i < 80 && !runtime.snapshot().accepted_fec_requests; ++i) {
      const auto at = 30000 + i * 50000;
      f.feedback(runtime, at, i % 8 == 0 ? 8 : 0, true, 1);
      const auto trace = f.wire.protection_trace(at + 10000);
      ASSERT_TRUE(runtime.process_interval(at + 10000, &trace));
    }
    const auto state = runtime.snapshot();
    ASSERT_GT(state.accepted_fec_requests, 0U);
    EXPECT_GT(state.infeasible_fec_decisions, 0U);
    EXPECT_EQ(state.fec_selection.classes[0].result, fec_selection_result_e::best_effort);
    EXPECT_FALSE(state.fec_selection.classes[0].target_met);
    EXPECT_EQ(f.policy->snapshot().accepted->fec_base, 5U);
    EXPECT_EQ(f.policy->snapshot().accepted->budget.total_kbps, 30000);
    EXPECT_EQ(f.policy->snapshot().applied, applied);
  }

  TEST(GoogCcRuntime, IndependentlyDisabledBitrateAndFecKeepTheUserPolicy) {
    runtime_fixture_t f;
    f.config.automatic_bitrate_enabled = false;
    f.config.automatic_fec_enabled = false;
    googcc_runtime_t runtime(f.config, f.policy);
    f.apply();
    f.feedback(runtime, 1000);
    ASSERT_TRUE(runtime.try_take_control(11000));
    const auto current = f.policy->snapshot().accepted;
    for (int i = 0; i < 50; ++i) {
      const auto at = 30000 + i * 50000;
      f.feedback(runtime, at, 6);
      const auto trace = f.wire.protection_trace(at + 10000);
      ASSERT_TRUE(runtime.process_interval(at + 10000, &trace));
      EXPECT_FALSE(runtime.needs_protection_trace(at + 10000));
    }
    EXPECT_EQ(f.policy->snapshot().accepted, current);
    EXPECT_EQ(runtime.snapshot().accepted_policy_requests, 0U);
    EXPECT_EQ(runtime.snapshot().accepted_fec_requests, 0U);
    EXPECT_LT(runtime.snapshot().estimate.target_kbps, 28500);
  }

  TEST(GoogCcRuntime, PendingOrBackendFailedPolicyCannotEnableNewAutomaticProtection) {
    for (const bool failed : { false, true }) {
      runtime_fixture_t f;
      f.config.automatic_bitrate_enabled = false;
      f.config.automatic_fec_enabled = true;
      googcc_runtime_t runtime(f.config, f.policy);
      f.apply();
      f.feedback(runtime, 1000);
      ASSERT_TRUE(runtime.try_take_control(11000));
      if (failed) {
        ASSERT_TRUE(f.policy->acknowledge_encoder(f.policy->acquire_pending(), policy_failure_e::backend_failure));
      }
      for (int i = 0; i < 50; ++i) {
        const auto at = 30000 + i * 50000;
        f.feedback(runtime, at, 2);
        const auto trace = f.wire.protection_trace(at + 10000);
        ASSERT_TRUE(runtime.process_interval(at + 10000, &trace));
      }
      EXPECT_EQ(runtime.snapshot().accepted_fec_requests, 0U);
      EXPECT_EQ(f.policy->snapshot().accepted->fec_base, 10U);
    }
  }

  TEST(GoogCcRuntime, ManualPreemptionRejectsFecEvenWithFreshProjection) {
    runtime_fixture_t f;
    f.config.automatic_bitrate_enabled = false;
    f.config.automatic_fec_enabled = true;
    googcc_runtime_t runtime(f.config, f.policy);
    f.apply();
    f.feedback(runtime, 1000);
    ASSERT_TRUE(runtime.try_take_control(11000));
    const auto current = f.policy->snapshot().accepted;
    ASSERT_EQ(f.policy->request_normalized(f.budget, 0, 0, 0, current->revision, current->control_epoch).result,
      policy_request_result_e::accepted);
    const auto manual = f.policy->snapshot().accepted;
    for (int i = 0; i < 50; ++i) {
      const auto at = 30000 + i * 50000;
      f.feedback(runtime, at, 2);
      const auto trace = f.wire.protection_trace(at + 10000);
      ASSERT_TRUE(runtime.process_interval(at + 10000, &trace));
    }
    EXPECT_EQ(f.policy->snapshot().accepted, manual);
    EXPECT_EQ(runtime.snapshot().accepted_fec_requests, 0U);
    EXPECT_FALSE(runtime.snapshot().lease);
  }

  TEST(GoogCcRuntime, FecFeedbackSilenceCannotManufactureStableCleanCoverage) {
    runtime_fixture_t f;
    f.config.automatic_bitrate_enabled = false;
    f.config.automatic_fec_enabled = true;
    googcc_runtime_t runtime(f.config, f.policy);
    f.apply();
    f.feedback(runtime, 1000);
    ASSERT_TRUE(runtime.try_take_control(11000));
    ASSERT_TRUE(f.policy->acknowledge_encoder(f.policy->acquire_pending(), policy_failure_e::none));
    const auto current = f.policy->snapshot().accepted;
    const auto trace = f.wire.protection_trace(20000000);
    ASSERT_TRUE(runtime.process_interval(20000000, &trace));
    EXPECT_TRUE(runtime.snapshot().feedback_stale);
    EXPECT_EQ(f.policy->snapshot().accepted, current);
    EXPECT_EQ(runtime.snapshot().accepted_fec_requests, 0U);
    EXPECT_FALSE(runtime.needs_protection_trace(20000000));
  }

  TEST(GoogCcRuntime, ActualCleanCoverageAcrossReservedIdsCanDownstepWithoutIncreasingBudget) {
    runtime_fixture_t f;
    const auto original = f.policy->snapshot().accepted;
    ASSERT_EQ(f.policy->request_normalized(f.budget, 20, 20, 20, original->revision, original->control_epoch).result,
      policy_request_result_e::accepted);
    f.config.automatic_bitrate_enabled = false;
    f.config.automatic_fec_enabled = true;
    f.config.fec.decrease_stability_us = 500000;
    googcc_runtime_t runtime(f.config, f.policy);
    f.apply();
    f.feedback(runtime, 1000);
    ASSERT_TRUE(runtime.try_take_control(11000));
    ASSERT_TRUE(f.policy->acknowledge_encoder(f.policy->acquire_pending(), policy_failure_e::none));
    const auto applied = f.policy->snapshot().applied;
    for (int i = 1; i <= 80 && !runtime.snapshot().accepted_fec_requests; ++i) {
      f.next_sequence += 11;  // Reserved, never submitted or reported.
      const auto at = i * 50000;
      f.feedback(runtime, at);
      const auto trace = f.wire.protection_trace(at + 10000);
      ASSERT_TRUE(runtime.process_interval(at + 10000, &trace));
    }
    ASSERT_GT(runtime.snapshot().accepted_fec_requests, 0U);
    EXPECT_EQ(f.policy->snapshot().accepted->fec_base, 15U);
    EXPECT_EQ(f.policy->snapshot().accepted->budget.total_kbps, f.budget.total_kbps);
    EXPECT_EQ(f.policy->snapshot().applied, applied);
    EXPECT_EQ(f.wire.snapshot().ledger.missing_declarations, 0U);
  }

  TEST(GoogCcRuntime, InvalidProjectionCannotAccumulateCleanResidenceBeforeLaterValidData) {
    runtime_fixture_t f;
    const auto original = f.policy->snapshot().accepted;
    ASSERT_EQ(f.policy->request_normalized(f.budget, 20, 20, 20, original->revision, original->control_epoch).result,
      policy_request_result_e::accepted);
    f.config.automatic_bitrate_enabled = false;
    f.config.automatic_fec_enabled = true;
    f.config.fec.decrease_stability_us = 500000;
    googcc_runtime_t runtime(f.config, f.policy);
    f.apply();
    f.feedback(runtime, 1000);
    ASSERT_TRUE(runtime.try_take_control(11000));
    ASSERT_TRUE(f.policy->acknowledge_encoder(f.policy->acquire_pending(), policy_failure_e::none));
    for (int i = 1; i <= 110; ++i) {
      const auto at = i * 50000;
      f.feedback(runtime, at);
      auto projection = f.wire.protection_trace(at + 10000);
      if (i <= 100 && !projection.samples.empty()) ++projection.samples.front().sent.protection.frame_data_shards;
      ASSERT_TRUE(runtime.process_interval(at + 10000, &projection));
    }
    EXPECT_EQ(runtime.snapshot().accepted_fec_requests, 0U);
    EXPECT_EQ(f.policy->snapshot().accepted->fec_base, 20U);
  }

  TEST(GoogCcRuntime, TruncatedSnapshotsCannotCountUnexaminedPacketsAsCleanResidence) {
    runtime_fixture_t f;
    const auto original = f.policy->snapshot().accepted;
    ASSERT_EQ(f.policy->request_normalized(f.budget, 20, 20, 20, original->revision, original->control_epoch).result,
      policy_request_result_e::accepted);
    f.config.automatic_bitrate_enabled = false;
    f.config.automatic_fec_enabled = true;
    f.config.fec.decrease_stability_us = 500000;
    googcc_runtime_t runtime(f.config, f.policy);
    f.apply();
    f.feedback(runtime, 1000);
    ASSERT_TRUE(runtime.try_take_control(11000));
    ASSERT_TRUE(f.policy->acknowledge_encoder(f.policy->acquire_pending(), policy_failure_e::none));
    for (int i = 1; i <= 120; ++i) {
      const auto at = i * 20000;
      f.feedback(runtime, at);
      if (i % 20 == 0) {
        // 400 new packets between reads, only 300 retained in each projection.
        // Internally clean windows are not continuous coverage across reads.
        const auto trace = f.wire.protection_trace(at + 10000, 2000000, 200000, 300);
        ASSERT_TRUE(runtime.process_interval(at + 10000, &trace));
      }
    }
    EXPECT_EQ(runtime.snapshot().accepted_fec_requests, 0U);
    EXPECT_EQ(f.policy->snapshot().accepted->fec_base, 20U);
  }

  TEST(GoogCcRuntime, ExplicitReenableNeedsNewAppliedPolicyAndNewMappedCoverage) {
    runtime_fixture_t f;
    f.config.budgeted_probing_enabled = true;
    googcc_runtime_t previous(f.config, f.policy);
    f.apply();
    f.feedback(previous, 1000);
    ASSERT_TRUE(previous.try_take_control(11000));
    const auto controlled = f.policy->snapshot().accepted;
    const auto old_sequence = f.next_sequence++;
    sent_packet_t delayed { old_sequence, 12000, 1280, 37, controlled->revision, packet_kind_e::data, {} };
    const auto send = f.wire.commit_success_event(delayed);
    ASSERT_TRUE(send);
    ASSERT_TRUE(previous.on_successful_send(*send));
    const auto manual = f.policy->request_normalized(controlled->budget, 10, 30, 20,
                                  controlled->revision, controlled->control_epoch)
                          .policy;
    ASSERT_TRUE(manual);
    const auto activation = f.policy->request_automatic_control(true, false, 50000,
                                      manual->revision, manual->control_epoch, "reenable")
                              .policy;
    ASSERT_TRUE(activation);
    EXPECT_FALSE(previous.probe_eligible(12000));
    ASSERT_TRUE(previous.process_interval(13000));
    EXPECT_FALSE(previous.snapshot().lease);
    EXPECT_TRUE(previous.snapshot().control_revoked);
    auto config = f.config;
    config.controller.start_time_us = 20000;
    config.controller.maximum_kbps = 48500;
    config.controller.first_probe_cluster_id = static_cast<std::int32_t>(previous.snapshot().estimate.last_generated_probe_cluster + 1);
    previous.stop();
    googcc_runtime_t current(config, f.policy, activation);
    EXPECT_FALSE(current.try_take_control(20000));
    const auto pending = f.policy->acquire_pending();
    ASSERT_EQ(pending, activation);
    ASSERT_TRUE(f.policy->acknowledge_encoder(pending, policy_failure_e::none));
    TF_PACKET_REPORT old {};
    old.connectionEpoch = 7;
    old.reportSequence = ++f.report_sequence;
    old.receiverClockEpoch = 1;
    old.receiverSampleTimeUs = 9022000;
    old.baseExtendedSequence = old_sequence;
    old.packetCount = 1;
    old.status[0] = TF_RECEIVED;
    old.firstArrivalTimeUs[0] = 9017000;
    std::array<std::uint8_t, TF_MAX_REPORT_BYTES> bytes {};
    const auto size = TfEncodeReport(&old, bytes.data(), bytes.size());
    ASSERT_GT(size, 0U);
    const auto feedback = f.wire.apply_wire_event(std::span(bytes.data(), size), 22000);
    ASSERT_EQ(feedback.feedback.result, report_result_e::accepted);
    ASSERT_TRUE(current.on_feedback(feedback));
    EXPECT_EQ(current.snapshot().estimate.unmapped_feedback_changes, 1U);
    EXPECT_EQ(current.snapshot().estimate.last_covered_send_us, -1);
    EXPECT_FALSE(current.try_take_control(22000));
    EXPECT_FALSE(current.probe_eligible(22000));
    f.feedback(current, 30000);
    ASSERT_TRUE(current.try_take_control(40000));
    EXPECT_GT(current.snapshot().lease->control_epoch, activation->control_epoch);
    EXPECT_EQ(f.policy->snapshot().accepted->automatic_control, activation->automatic_control);
    const auto probes = current.take_probe_requests(40000);
    ASSERT_FALSE(probes.empty());
    EXPECT_GT(probes.front().cluster_id, previous.snapshot().estimate.last_generated_probe_cluster);
  }

  TEST(GoogCcRuntime, ActivationSnapshotCannotBeSilentlyReplacedOrConfiguredWithDifferentModes) {
    runtime_fixture_t f;
    const auto manual = f.policy->snapshot().accepted;
    const auto activation = f.policy->request_automatic_control(true, false, 30000,
                                      manual->revision, manual->control_epoch, "enable")
                              .policy;
    ASSERT_TRUE(activation);
    auto incorrect = f.config;
    incorrect.automatic_fec_enabled = true;
    EXPECT_THROW(googcc_runtime_t runtime(incorrect, f.policy, activation), std::invalid_argument);
    const auto newer = f.policy->request_automatic_control(true, false, 30000,
                                 activation->revision, activation->control_epoch, "replace")
                         .policy;
    ASSERT_TRUE(newer);
    googcc_runtime_t runtime(f.config, f.policy, activation);
    f.apply();
    f.feedback(runtime, 1000);
    EXPECT_FALSE(runtime.try_take_control(11000));
    EXPECT_TRUE(runtime.snapshot().control_revoked);
    EXPECT_EQ(f.policy->snapshot().accepted, newer);
  }
}  // namespace
