#include "src/transport_fec_controller.h"
#include "src/transport_feedback_wire.h"

#include <gtest/gtest.h>

namespace {
  using namespace transport;

  sent_packet_t
  packet(std::uint64_t sequence, std::int64_t time) {
    sent_packet_t result { sequence, time, 1280, 1, 2, packet_kind_e::data, {} };
    result.protection = { 20, 20, static_cast<std::uint16_t>(sequence % 20), 0, protection_class_e::base, 20, 1 };
    return result;
  }

  protection_trace_t
  trace(bool burst = false) {
    protection_trace_t result;
    result.connection_epoch = 7;
    result.sampled_at_us = 2000000;
    result.settled_until_us = 1800000;
    result.last_feedback_us = 1950000;
    result.valid = true;
    for (std::uint64_t i = 0; i < 640; ++i) {
      const bool lost = burst ? i % 160 < 8 : i % 20 == 0;
      result.samples.push_back({ packet(i, 1000000 + static_cast<std::int64_t>(i) * 1000),
        lost ? packet_status_e::missing : packet_status_e::received, false, i + 1 });
    }
    return result;
  }

  frame_policy_t
  policy() {
    frame_policy_t result;
    result.connection_epoch = 7;
    result.basis = budget_basis_e::normalized;
    result.budget.total_kbps = 10000;
    result.budget.other_kbps = 300;
    result.budget.video_overhead_kbps = 500;
    return result;
  }

  TEST(ProtectionLedger, RejectsInconsistentShardAndFrameMetadataWithoutCommitting) {
    send_ledger_t ledger(7);
    auto invalid = packet(0, 1000);
    invalid.protection.shard_index = 20;
    EXPECT_FALSE(ledger.commit_success(invalid));
    invalid = packet(0, 1000);
    invalid.protection.total_shards = 256;
    EXPECT_FALSE(ledger.commit_success(invalid));
    invalid = packet(0, 1000);
    invalid.protection.block_index = 4;
    EXPECT_FALSE(ledger.commit_success(invalid));
    invalid = packet(0, 1000);
    invalid.protection.frame_class = static_cast<protection_class_e>(3);
    EXPECT_FALSE(ledger.commit_success(invalid));
    EXPECT_EQ(ledger.snapshot().committed_packets, 0U);
    EXPECT_TRUE(ledger.commit_success(packet(0, 1000)));
  }

  TEST(ProtectionLedger, ProjectionHasOnlyActualMatureReceiptsAndNeverInfersMissingGaps) {
    send_ledger_t ledger(7);
    ASSERT_TRUE(ledger.commit_success(packet(0, 1000)));
    ASSERT_TRUE(ledger.commit_success(packet(10, 2000)));
    ASSERT_TRUE(ledger.commit_success(packet(11, 250000)));
    const auto result = ledger.protection_trace(300000);
    ASSERT_TRUE(result.valid);
    ASSERT_EQ(result.samples.size(), 2U);
    EXPECT_EQ(result.samples[1].sent.extended_sequence, 10U);
    EXPECT_EQ(result.samples[0].commit_ordinal, 1U);
    EXPECT_EQ(result.samples[1].commit_ordinal, 2U);
    EXPECT_EQ(result.samples[0].status, packet_status_e::pending);
    EXPECT_EQ(ledger.snapshot().missing_declarations, 0U);
    EXPECT_EQ(ledger.snapshot().committed_packets, 3U);
  }

  TEST(ProtectionLedger, LateCorrectionIsProjectedOnceWithoutChangingRawAccounting) {
    send_ledger_t ledger(7);
    ASSERT_TRUE(ledger.commit_success(packet(0, 1000)));
    std::array<packet_observation_t, 1> observations { { { 0, packet_status_e::missing, -1 } } };
    ASSERT_EQ(ledger.apply({ 7, 1, 1, 50000, observations, 1000000 }).result, report_result_e::accepted);
    observations[0] = { 0, packet_status_e::received, 950000 };
    ASSERT_EQ(ledger.apply({ 7, 2, 1, 100000, observations, 1100000 }).result, report_result_e::accepted);
    ASSERT_EQ(ledger.apply({ 7, 3, 1, 150000, observations, 1150000 }).result, report_result_e::accepted);
    const auto result = ledger.protection_trace(300000);
    ASSERT_EQ(result.samples.size(), 1U);
    EXPECT_EQ(result.samples[0].status, packet_status_e::received);
    EXPECT_TRUE(result.samples[0].late_correction);
    EXPECT_EQ(ledger.snapshot().received_packets, 1U);
    EXPECT_EQ(ledger.snapshot().missing_declarations, 1U);
    EXPECT_EQ(ledger.snapshot().late_corrections, 1U);
  }

  TEST(ProtectionLedger, BoundedProjectionAndClockReplacementDeclareUnknown) {
    send_ledger_t ledger(7);
    for (std::uint64_t i = 0; i < 4; ++i) ASSERT_TRUE(ledger.commit_success(packet(i, 1000 + i)));
    std::array<packet_observation_t, 1> observations { { { 0, packet_status_e::received, 900000 } } };
    ASSERT_EQ(ledger.apply({ 7, 1, 1, 50000, observations, 1000000 }).result, report_result_e::accepted);
    observations[0] = { 1, packet_status_e::missing, -1 };
    ASSERT_EQ(ledger.apply({ 7, 2, 2, 100000, observations, 1100000 }).result, report_result_e::accepted);
    const auto full = ledger.protection_trace(300000);
    EXPECT_EQ(full.samples[0].status, packet_status_e::unknown);
    const auto bounded = ledger.protection_trace(300000, 2000000, 200000, 2);
    EXPECT_TRUE(bounded.history_truncated);
    ASSERT_EQ(bounded.samples.size(), 2U);
    EXPECT_EQ(bounded.samples[0].sent.extended_sequence, 2U);
    EXPECT_FALSE(ledger.protection_trace(1000).valid);
    EXPECT_FALSE(ledger.protection_trace(300000, 100000, 200000).valid);
    EXPECT_FALSE(ledger.protection_trace(300000, 2000000, 200000, 4097).valid);
  }

  TEST(FecSelection, SameAverageLossNeedsDifferentProtectionForBurstAndScatteredTraces) {
    const auto scattered = select_fec(trace(), policy());
    const auto burst = select_fec(trace(true), policy());
    ASSERT_TRUE(scattered.valid && burst.valid);
    ASSERT_TRUE(scattered.changed && burst.changed);
    EXPECT_EQ(scattered.classes[0].percentage, 10U);
    EXPECT_EQ(burst.classes[0].percentage, 40U);
    EXPECT_EQ(scattered.classes[0].replay_failures, 0U);
    EXPECT_EQ(burst.classes[0].replay_failures, 0U);
    auto budget = policy().budget;
    budget.fec_numerator = burst.classes[0].percentage;
    const auto allocation = allocate_budget(budget);
    ASSERT_TRUE(allocation);
    EXPECT_EQ(allocation->wire_budget_kbps, 10000);
    EXPECT_LT(allocation->encoder_kbps, allocate_budget(policy().budget)->encoder_kbps);
  }

  TEST(FecSelection, AllMissingReportsInfeasibleWithoutPromisingRecoveryOrRaisingBudget) {
    auto input = trace();
    for (auto &sample : input.samples) sample.status = packet_status_e::missing;
    auto current = policy();
    current.fec_base = 20;
    const auto result = select_fec(input, current);
    ASSERT_TRUE(result.valid);
    EXPECT_TRUE(result.infeasible);
    EXPECT_FALSE(result.changed);
    EXPECT_EQ(result.classes[0].percentage, 20U);
    EXPECT_EQ(result.classes[0].result, fec_selection_result_e::infeasible);
  }

  TEST(FecSelection, PendingUnknownAndAbsentMetadataCannotCreateCleanEvidence) {
    for (int scenario = 0; scenario < 3; ++scenario) {
      auto input = trace();
      for (std::size_t i = 0; i < input.samples.size(); ++i) {
        auto &sample = input.samples[i];
        if (scenario == 0) sample.status = packet_status_e::pending;
        if (scenario == 1) sample.status = packet_status_e::unknown;
        if (scenario == 2) sample.sent.protection = {};
      }
      auto current = policy();
      current.fec_base = 20;
      const auto result = select_fec(input, current, {}, { 20000000, 20000000 });
      ASSERT_TRUE(result.valid);
      EXPECT_FALSE(result.changed);
      EXPECT_EQ(result.classes[0].result, fec_selection_result_e::insufficient);
    }
  }

  TEST(FecSelection, SilenceStaleFeedbackForeignEpochAndInvalidConfigurationDoNotSelect) {
    auto input = trace();
    input.last_feedback_us = 0;
    EXPECT_FALSE(select_fec(input, policy()).valid);
    input = trace();
    input.connection_epoch = 8;
    EXPECT_FALSE(select_fec(input, policy()).valid);
    input = trace();
    fec_selection_config_t config;
    config.minimum_windows = 1;
    EXPECT_FALSE(select_fec(input, policy(), config).valid);
    input.samples[0].sent.protection.total_shards = 0;
    EXPECT_FALSE(select_fec(input, policy()).valid);
  }

  TEST(FecSelection, DownwardStepRequiresNewCleanCoverageAndResidenceTime) {
    auto input = trace();
    for (auto &sample : input.samples) sample.status = packet_status_e::received;
    auto current = policy();
    current.fec_base = 50;
    EXPECT_FALSE(select_fec(input, current).changed);
    EXPECT_FALSE(select_fec(input, current, {}, { 10000000, 9999999 }).changed);
    EXPECT_FALSE(select_fec(input, current, {}, { 9999999, 10000000 }).changed);
    const auto result = select_fec(input, current, {}, { 10000000, 10000000 });
    EXPECT_TRUE(result.changed);
    EXPECT_EQ(result.classes[0].percentage, 40U);
  }

  TEST(FecSelection, LateCorrectionsStayConservativeAndBlockCleanDownwardEvidence) {
    auto input = trace();
    for (auto &sample : input.samples) {
      sample.late_correction = sample.status == packet_status_e::missing;
      sample.status = packet_status_e::received;
    }
    auto current = policy();
    current.fec_base = 20;
    EXPECT_FALSE(select_fec(input, current, {}, { 20000000, 20000000 }).changed);
    const auto selected = select_fec(input, policy());
    EXPECT_TRUE(selected.changed);
    EXPECT_EQ(selected.classes[0].percentage, 10U);
  }

  TEST(FecSelection, ExhaustedBudgetCannotBuyProtectionWithExtraNetworkLoad) {
    auto current = policy();
    current.budget.total_kbps = 1;
    current.budget.other_kbps = current.budget.video_overhead_kbps = 0;
    const auto result = select_fec(trace(), current);
    EXPECT_TRUE(result.valid);
    EXPECT_TRUE(result.infeasible);
    EXPECT_FALSE(result.changed);
    EXPECT_EQ(result.classes[0].percentage, 0U);
  }

  TEST(FecSelection, CandidateRepartitionsTheWholeFrameInsteadOfRejectingOld255ShardBlock) {
    auto input = trace();
    for (auto &sample : input.samples) {
      sample.sent.protection.data_shards = sample.sent.protection.total_shards = 255;
      sample.sent.protection.shard_index %= 255;
      sample.sent.protection.frame_data_shards = 255;
    }
    const auto result = select_fec(input, policy());
    EXPECT_TRUE(result.valid);
    EXPECT_FALSE(result.infeasible);
    EXPECT_TRUE(result.changed);
    EXPECT_TRUE(result.classes[0].target_met);
    EXPECT_EQ(result.classes[0].percentage, 5U);
    EXPECT_EQ(plan_fec_frame(255, result.classes[0].percentage, 0)->block_count, 2U);
  }

  TEST(FecSelection, BestEffortReducesBurstFrameRiskWithoutClaimingStrictTarget) {
    auto input = trace(true);
    for (auto &sample : input.samples) sample.sent.protection = { 1, 1, 0, 0, protection_class_e::base, 1, 1 };
    fec_selection_config_t config;
    config.minimum_parity = 2;
    const auto result = select_fec(input, policy(), config);
    ASSERT_TRUE(result.valid && result.changed);
    EXPECT_TRUE(result.infeasible);
    EXPECT_FALSE(result.classes[0].target_met);
    EXPECT_EQ(result.classes[0].result, fec_selection_result_e::best_effort);
    EXPECT_EQ(result.classes[0].percentage, 5U);
    EXPECT_GT(result.classes[0].worst_frame_failure_ppm, 1000U);
    EXPECT_LT(result.classes[0].worst_frame_failure_ppm, 50000U);
    auto protected_policy = policy();
    protected_policy.fec_base = 5;
    const auto again = select_fec(input, protected_policy, config);
    EXPECT_FALSE(again.changed);  // Equal risk cannot justify more overhead.
    EXPECT_TRUE(again.infeasible);
  }

  TEST(FecSelection, ReplayCountsAnyFailedBlockAsOneFailedWholeFrame) {
    auto input = trace();
    for (std::size_t i = 0; i < input.samples.size(); ++i) {
      auto &sample = input.samples[i];
      sample.sent.protection = { 150, 180, static_cast<std::uint16_t>(i % 180),
        static_cast<std::uint8_t>((i / 180) % 2), protection_class_e::base, 300, 2 };
      sample.sent.kind = i % 180 < 150 ? packet_kind_e::data : packet_kind_e::fec;
      sample.status = i % 400 < 40 ? packet_status_e::missing : packet_status_e::received;
    }
    auto current = policy();
    current.fec_base = 20;
    fec_selection_config_t config;
    config.maximum_replay_failure_ppm[0] = 1000000;
    const auto result = select_fec(input, current, config);
    ASSERT_TRUE(result.valid);
    ASSERT_EQ(result.classes[0].percentage, 20U);  // No clean downstep.
    std::size_t failures = 0, first_failures = 0;
    for (std::size_t begin = 0; begin + 360 <= input.samples.size(); ++begin) {
      std::size_t lost1 = 0, lost2 = 0;
      for (std::size_t j = 0; j < 360; ++j) {
        if (input.samples[begin + j].status == packet_status_e::missing) {
          if (j < 180)
            ++lost1;
          else
            ++lost2;
        }
      }
      first_failures += lost1 > 30;
      failures += lost1 > 30 || lost2 > 30;
    }
    EXPECT_EQ(result.classes[0].replay_windows, 281U);
    EXPECT_EQ(result.classes[0].replay_failures, failures);
    EXPECT_GT(failures, first_failures);
  }

  TEST(FecSelection, MissingWholeFrameGeometryIsInsufficientAndInconsistentGeometryInvalid) {
    auto input = trace();
    for (auto &sample : input.samples) {
      sample.sent.protection.frame_data_shards = 0;
      sample.sent.protection.frame_blocks = 0;
    }
    EXPECT_EQ(select_fec(input, policy()).classes[0].result, fec_selection_result_e::insufficient);
    input = trace();
    input.samples[0].sent.protection.frame_data_shards = 21;
    EXPECT_FALSE(select_fec(input, policy()).valid);
    send_ledger_t ledger(7);
    EXPECT_FALSE(ledger.commit_success(input.samples[0].sent));
    input = trace();
    input.samples[0].sent.protection.frame_blocks = 5;
    EXPECT_FALSE(select_fec(input, policy()).valid);
    EXPECT_FALSE(ledger.commit_success(input.samples[0].sent));
  }

  TEST(FecSelection, ProtectedFallbackCannotPretendAnUnrepresentableFrameMeetsTarget) {
    auto input = trace();
    for (auto &sample : input.samples) sample.sent.protection = { 1023, 1023, 0, 0, protection_class_e::base, 4092, 4 };
    const auto result = select_fec(input, policy());
    EXPECT_TRUE(result.valid);
    EXPECT_FALSE(result.changed);
    EXPECT_FALSE(result.classes[0].target_met);
    EXPECT_EQ(result.classes[0].result, fec_selection_result_e::insufficient);  // Fewer than one complete replay window.
  }

  TEST(ProtectionLedger, OrdinalsSurviveEvictionAndRejectedCommitWithoutCountingReservedIds) {
    send_ledger_t ledger(7, 2);
    ASSERT_TRUE(ledger.commit_success(packet(0, 1000)));
    ASSERT_TRUE(ledger.commit_success(packet(10, 2000)));
    ASSERT_TRUE(ledger.commit_success(packet(20, 3000)));
    EXPECT_FALSE(ledger.commit_success(packet(19, 4000)));
    ASSERT_TRUE(ledger.commit_success(packet(100, 4000)));
    const auto projected = ledger.protection_trace(300000);
    ASSERT_EQ(projected.samples.size(), 2U);
    EXPECT_EQ(projected.samples[0].commit_ordinal, 3U);
    EXPECT_EQ(projected.samples[1].commit_ordinal, 4U);
    EXPECT_EQ(ledger.snapshot().committed_packets, 4U);
    EXPECT_EQ(ledger.snapshot().missing_declarations, 0U);
  }

  TEST(FecSelection, ReservedUnsentIdentitiesDoNotBreakActualSuccessfulCoverage) {
    auto input = trace();
    for (std::size_t i = 0; i < input.samples.size(); ++i) input.samples[i].sent.extended_sequence = i * 2;
    const auto result = select_fec(input, policy());
    ASSERT_TRUE(result.valid);
    EXPECT_EQ(result.classes[0].percentage, 10U);
    EXPECT_TRUE(result.classes[0].target_met);
    EXPECT_EQ(result.classes[0].replay_failures, 0U);
  }

  TEST(FecSelection, MissingSuccessfulCoverageAndInvalidOrdinalsCannotLicenseCleanDownstep) {
    auto input = trace();
    for (std::size_t i = 0; i < input.samples.size(); ++i) {
      input.samples[i].status = packet_status_e::received;
      input.samples[i].commit_ordinal = i * 2 + 1;
    }
    auto current = policy();
    current.fec_base = 20;
    const auto result = select_fec(input, current, {}, { 20000000, 20000000 });
    EXPECT_TRUE(result.valid);
    EXPECT_FALSE(result.changed);
    EXPECT_EQ(result.classes[0].result, fec_selection_result_e::insufficient);
    input.samples[0].commit_ordinal = 0;
    EXPECT_FALSE(select_fec(input, current).valid);
    input = trace();
    input.samples[1].commit_ordinal = input.samples[0].commit_ordinal;
    EXPECT_FALSE(select_fec(input, current).valid);
  }

  TEST(FecSelection, SharedGeometryDoesNotShareDifferentClassTargets) {
    auto input = trace();
    for (std::size_t i = 0; i < input.samples.size(); ++i)
      input.samples[i].sent.protection.frame_class = static_cast<protection_class_e>(i % 3);
    fec_selection_config_t config;
    config.maximum_replay_failure_ppm = { 1000000, 100, 1000000 };
    const auto result = select_fec(input, policy(), config);
    ASSERT_TRUE(result.valid);
    EXPECT_EQ(result.classes[0].percentage, 0U);
    EXPECT_EQ(result.classes[1].percentage, 10U);
    EXPECT_EQ(result.classes[2].percentage, 0U);
    EXPECT_TRUE(result.classes[0].target_met && result.classes[1].target_met && result.classes[2].target_met);
  }

  TEST(FecSelection, LaterCleanFeedbackCannotReuseEarlierLossEvenWithIdenticalGeometry) {
    auto input = trace(true);
    EXPECT_EQ(select_fec(input, policy()).classes[0].percentage, 40U);
    for (auto &sample : input.samples) sample.status = packet_status_e::received;
    const auto clean = select_fec(input, policy());
    EXPECT_FALSE(clean.changed);
    EXPECT_EQ(clean.classes[0].percentage, 0U);
    EXPECT_EQ(clean.classes[0].replay_failures, 0U);
    EXPECT_TRUE(clean.classes[0].target_met);
  }
}  // namespace
