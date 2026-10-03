#include "src/transport_policy.h"

#include <atomic>
#include <gtest/gtest.h>
#include <thread>

namespace {
  transport::frame_policy_t
  initial(std::uint64_t epoch = 42) {
    transport::frame_policy_t p;
    p.connection_epoch = epoch;
    p.budget.total_kbps = 40000;
    p.encoder_kbps = 32000;
    p.fec_base = p.fec_key = p.fec_recovery = 20;
    return p;
  }
  void
  ready(transport::policy_state_t &state) {
    auto p = state.begin_encoder_initialization();
    ASSERT_NE(p, nullptr);
    ASSERT_TRUE(state.acknowledge_encoder(p, transport::policy_failure_e::none));
  }
}  // namespace

TEST(TransportPolicy, LiveModeRequestsRequireNegotiationAndANormalizedSession) {
  transport::policy_state_t old(initial(), 50000);
  auto p = old.request_normalized(initial().budget, 20, 20, 20, 1, 1).policy;
  ASSERT_TRUE(p);
  EXPECT_EQ(old.request_automatic_control(true, true, 50000, p->revision, p->control_epoch, "modes").result,
    transport::policy_request_result_e::invalid);
  transport::policy_state_t negotiated(initial(), 50000, true, true);
  EXPECT_EQ(negotiated.request_automatic_control(true, true, 50000, 1, 1, "modes").result,
    transport::policy_request_result_e::invalid);
}

TEST(TransportPolicy, EveryExplicitModeRequestRevokesThePreviousGenerationBeforeApply) {
  transport::policy_state_t state(initial(), 50000, true, true);
  ready(state);
  auto manual = state.request_normalized(initial().budget, 20, 30, 20, 1, 1).policy;
  ASSERT_TRUE(manual);
  auto armed = state.request_automatic_control(true, true, 50000, manual->revision, manual->control_epoch, "enable").policy;
  ASSERT_TRUE(armed);
  EXPECT_EQ(armed->control_source, transport::control_source_e::manual);
  EXPECT_GT(armed->control_epoch, manual->control_epoch);
  ASSERT_TRUE(armed->automatic_control);
  EXPECT_EQ(armed->automatic_control->activation_epoch, armed->control_epoch);
  EXPECT_EQ(armed->budget.total_kbps, 40000);
  EXPECT_EQ(state.active()->revision, 1u);
  EXPECT_FALSE(state.acknowledge_first_sent(armed, 10));
  // Even a byte-identical old request cannot renew a revoked generation.
  EXPECT_EQ(state.request_automatic_control(true, true, 50000, manual->revision, manual->control_epoch, "enable").result,
    transport::policy_request_result_e::conflict);
  auto next = state.request_automatic_control(false, false, 25000, armed->revision, armed->control_epoch, "disable").policy;
  ASSERT_TRUE(next);
  EXPECT_GT(next->control_epoch, armed->control_epoch);
  EXPECT_EQ(next->budget.total_kbps, 25000);
  EXPECT_FALSE(next->automatic_control->bitrate);
  EXPECT_FALSE(next->automatic_control->fec);
  EXPECT_EQ(state.acquire_pending(), next);
  ASSERT_TRUE(state.acknowledge_encoder(next, transport::policy_failure_e::none));
  ASSERT_TRUE(state.acknowledge_first_sent(next, 11));
  EXPECT_EQ(state.active(), next);
  EXPECT_FALSE(state.acknowledge_encoder(armed, transport::policy_failure_e::none));
}

TEST(TransportPolicy, ControllerCannotBypassAnyOfTheFourModeCombinations) {
  for (const bool bitrate : { false, true }) {
    for (const bool fec : { false, true }) {
      SCOPED_TRACE(::testing::Message() << "bitrate=" << bitrate << " fec=" << fec);
      transport::policy_state_t state(initial(), 50000, true, true);
      const auto manual = state.request_normalized(initial().budget, 20, 30, 20, 1, 1).policy;
      const auto armed = state.request_automatic_control(bitrate, fec, 30000, manual->revision, manual->control_epoch, "modes").policy;
      ASSERT_TRUE(armed);
      const auto granted = state.transfer_control({ 42, armed->control_epoch, transport::control_source_e::manual },
                                  transport::control_source_e::googcc, armed->revision)
                             .policy;
      ASSERT_TRUE(granted);
      EXPECT_EQ(granted->automatic_control, armed->automatic_control);
      const transport::control_lease_t lease { 42, granted->control_epoch, transport::control_source_e::googcc };
      auto budget = granted->budget;
      budget.total_kbps = 30001;
      EXPECT_EQ(state.request_controller_update(lease, budget, 20, 30, 20, granted->revision).result,
        transport::policy_request_result_e::invalid);
      budget.total_kbps = 25000;
      EXPECT_EQ(state.request_controller_update(lease, budget, 20, 30, 20, granted->revision).result,
        bitrate ? transport::policy_request_result_e::accepted : transport::policy_request_result_e::invalid);
      const auto current = state.snapshot().accepted;
      EXPECT_EQ(state.request_controller_update(lease, current->budget, 30, 40, 30, current->revision).result,
        fec ? transport::policy_request_result_e::accepted : transport::policy_request_result_e::invalid);
      EXPECT_EQ(state.snapshot().accepted->automatic_control, armed->automatic_control);
    }
  }
}

TEST(TransportPolicy, LiveCeilingValidationCannotExhaustReservesOrIncreaseAutomaticLoad) {
  transport::policy_state_t state(initial(), 50000, true, true);
  auto budget = initial().budget;
  budget.other_kbps = 2000;
  budget.video_overhead_kbps = 500;
  const auto manual = state.request_normalized(budget, 20, 30, 20, 1, 1).policy;
  ASSERT_TRUE(manual);
  for (const int invalid : { -1, 0, 2500, 50001, 800001 })
    EXPECT_EQ(state.request_automatic_control(true, true, invalid, manual->revision, manual->control_epoch, "invalid").result,
      transport::policy_request_result_e::invalid);
  EXPECT_EQ(state.snapshot().accepted, manual);
  const auto raised = state.request_automatic_control(true, false, 50000, manual->revision, manual->control_epoch, "raise").policy;
  ASSERT_TRUE(raised);
  EXPECT_EQ(raised->budget.total_kbps, 40000);
  const auto shrunk = state.request_automatic_control(true, false, 10000, raised->revision, raised->control_epoch, "shrink").policy;
  ASSERT_TRUE(shrunk);
  EXPECT_EQ(shrunk->budget.total_kbps, 10000);
  EXPECT_EQ(shrunk->budget.other_kbps, 2000);
}

TEST(TransportPolicy, ManualPolicyDisarmsAutomaticIntentAndOldLeasesStayRevoked) {
  transport::policy_state_t state(initial(), 50000, true, true);
  auto manual = state.request_normalized(initial().budget, 20, 30, 20, 1, 1).policy;
  auto armed = state.request_automatic_control(true, true, 50000, manual->revision, manual->control_epoch, "enable").policy;
  ASSERT_TRUE(armed);
  const auto granted = state.transfer_control({ 42, armed->control_epoch, transport::control_source_e::manual },
                              transport::control_source_e::googcc, armed->revision)
                         .policy;
  ASSERT_TRUE(granted);
  const transport::control_lease_t old { 42, granted->control_epoch, transport::control_source_e::googcc };
  const auto reduced = state.request_controller_update(old, granted->budget, 20, 30, 20, granted->revision, {}, 1000).policy;
  ASSERT_TRUE(reduced);
  const auto user = state.request_normalized(reduced->budget, 10, 10, 10, reduced->revision, reduced->control_epoch).policy;
  ASSERT_TRUE(user);
  EXPECT_FALSE(user->automatic_control);
  EXPECT_FALSE(user->encoder_ceiling_kbps);
  EXPECT_EQ(state.request_controller_update(old, granted->budget, 20, 30, 20, user->revision).result,
    transport::policy_request_result_e::conflict);
  const auto rearmed = state.request_automatic_control(true, false, 50000, user->revision, user->control_epoch, "reenable").policy;
  ASSERT_TRUE(rearmed);
  EXPECT_GT(rearmed->automatic_control->activation_epoch, armed->automatic_control->activation_epoch);
  EXPECT_FALSE(rearmed->encoder_ceiling_kbps);
  state.stop();
  EXPECT_EQ(state.request_automatic_control(true, true, 40000, rearmed->revision, rearmed->control_epoch, "stopped").result,
    transport::policy_request_result_e::stopped);
}

TEST(TransportPolicy, AcceptanceIsNotApplicationOrSend) {
  transport::policy_state_t state(initial(), 50000);
  EXPECT_FALSE(state.snapshot().encoder_initialized);
  EXPECT_FALSE(state.acquire_pending());
  EXPECT_FALSE(state.acknowledge_encoder(state.active(), transport::policy_failure_e::none));
  EXPECT_FALSE(state.acknowledge_first_sent(state.active(), 1));
  ready(state);
  auto r = state.request_legacy_change(30000, std::nullopt);
  ASSERT_EQ(r.result, transport::policy_request_result_e::accepted);
  EXPECT_EQ(state.active()->revision, 1u);
  EXPECT_FALSE(state.acknowledge_first_sent(r.policy, 2));
  EXPECT_EQ(state.acquire_pending(), r.policy);
  EXPECT_TRUE(state.acknowledge_encoder(r.policy, transport::policy_failure_e::none));
  EXPECT_FALSE(state.acknowledge_encoder(r.policy, transport::policy_failure_e::none));
  EXPECT_TRUE(state.acknowledge_first_sent(r.policy, 3));
  EXPECT_TRUE(state.acknowledge_first_sent(r.policy, 4));
  EXPECT_EQ(state.snapshot().receipts.back().first_sent_frame, 3u);
}

TEST(TransportPolicy, EncoderCeilingIsASeparateImmutableAppliedBoundaryAndManualClearsIt) {
  transport::policy_state_t state(initial(), 50000);
  ready(state);
  const auto manual = state.request_normalized(initial().budget, 20, 30, 20, 1, 1).policy;
  ASSERT_TRUE(manual);
  const auto granted = state.transfer_control({ 42, manual->control_epoch, transport::control_source_e::manual },
                              transport::control_source_e::googcc, manual->revision)
                         .policy;
  ASSERT_TRUE(granted);
  const transport::control_lease_t lease { 42, granted->control_epoch, transport::control_source_e::googcc };
  const auto cap = state.request_controller_update(lease, granted->budget, 20, 30, 20, granted->revision, "cap", 1500);
  ASSERT_EQ(cap.result, transport::policy_request_result_e::accepted);
  EXPECT_EQ(cap.policy->budget, granted->budget);
  EXPECT_EQ(cap.policy->encoder_ceiling_kbps, 1500);
  EXPECT_EQ(cap.policy->encoder_kbps, 1500);
  EXPECT_EQ(state.active()->encoder_kbps, 32000);
  const auto pending = state.acquire_pending();
  ASSERT_EQ(pending, cap.policy);
  ASSERT_TRUE(state.acknowledge_encoder(pending, transport::policy_failure_e::none));
  EXPECT_EQ(state.active()->encoder_kbps, 1500);
  EXPECT_TRUE(state.acknowledge_first_sent(cap.policy, 10));
  const auto requested = state.request_normalized(granted->budget, 20, 30, 20, cap.policy->revision, cap.policy->control_epoch);
  ASSERT_EQ(requested.result, transport::policy_request_result_e::accepted);
  EXPECT_FALSE(requested.policy->encoder_ceiling_kbps);
  EXPECT_EQ(requested.policy->encoder_kbps, granted->encoder_kbps);
  EXPECT_EQ(requested.policy->control_source, transport::control_source_e::manual);
  EXPECT_EQ(state.active(), cap.policy);
  EXPECT_EQ(state.request_controller_update(lease, granted->budget, 20, 30, 20, granted->revision, "cap", 1500).result,
    transport::policy_request_result_e::conflict);
}

TEST(TransportPolicy, EncoderCeilingParticipatesInIdempotencyAndCannotMintEncoderOrNetworkBudget) {
  transport::policy_state_t state(initial(), 50000);
  const auto manual = state.request_normalized(initial().budget, 20, 30, 20, 1, 1).policy;
  const auto granted = state.transfer_control({ 42, manual->control_epoch, transport::control_source_e::manual },
                              transport::control_source_e::googcc, manual->revision)
                         .policy;
  const transport::control_lease_t lease { 42, granted->control_epoch, transport::control_source_e::googcc };
  for (const int value : { 0, -1, 800001 }) {
    EXPECT_EQ(state.request_controller_update(lease, granted->budget, 20, 30, 20, granted->revision, {}, value).result,
      transport::policy_request_result_e::invalid);
  }
  const auto result = state.request_controller_update(lease, granted->budget, 20, 30, 20, granted->revision, "cap", 800000);
  ASSERT_EQ(result.result, transport::policy_request_result_e::accepted);
  EXPECT_EQ(result.policy->encoder_kbps, granted->encoder_kbps);
  EXPECT_EQ(result.policy->budget, granted->budget);
  EXPECT_EQ(state.request_controller_update(lease, granted->budget, 20, 30, 20, granted->revision, "cap", 800000).policy,
    result.policy);
  EXPECT_EQ(state.request_controller_update(lease, granted->budget, 20, 30, 20, granted->revision, "cap", 1500).result,
    transport::policy_request_result_e::conflict);
}

TEST(TransportPolicy, NegotiatedStartupNormalizesWithoutGrantOrFakeApply) {
  transport::policy_state_t state(initial(), 50000, true, true);
  const auto before = state.snapshot();
  auto budget = before.accepted->budget;
  budget.other_kbps = budget.video_overhead_kbps = 500;
  const auto prepared = state.prepare_normalized_controller({ 42, 1, transport::control_source_e::legacy }, budget, 20, 30, 25, 1);
  ASSERT_EQ(prepared.result, transport::policy_request_result_e::accepted);
  EXPECT_EQ(prepared.policy->basis, transport::budget_basis_e::normalized);
  EXPECT_EQ(prepared.policy->control_source, transport::control_source_e::legacy);
  EXPECT_EQ(prepared.policy->control_epoch, 1U);
  EXPECT_EQ(state.active(), before.applied);
  EXPECT_FALSE(state.snapshot().encoder_initialized);
  EXPECT_FALSE(state.acknowledge_first_sent(prepared.policy, 1));
  EXPECT_EQ(state.request_legacy_change(30000, {}).result, transport::policy_request_result_e::conflict);
  EXPECT_TRUE(state.snapshot().experimental_packet_control_negotiated);
  EXPECT_TRUE(state.snapshot().experimental_video_pacer_enabled);
}

TEST(TransportPolicy, StartupPreparationIsSingleUseBoundedAndEpochChecked) {
  transport::policy_state_t state(initial(), 50000);
  auto budget = initial().budget;
  auto too_high = budget;
  too_high.total_kbps = 41000;
  EXPECT_EQ(state.prepare_normalized_controller({ 42, 1, transport::control_source_e::legacy }, too_high, 20, 20, 20, 1).result,
    transport::policy_request_result_e::invalid);
  EXPECT_EQ(state.prepare_normalized_controller({ 43, 1, transport::control_source_e::legacy }, budget, 20, 20, 20, 1).result,
    transport::policy_request_result_e::conflict);
  EXPECT_EQ(state.prepare_normalized_controller({ 42, 1, transport::control_source_e::manual }, budget, 20, 20, 20, 1).result,
    transport::policy_request_result_e::conflict);
  EXPECT_EQ(state.prepare_normalized_controller({ 42, 2, transport::control_source_e::legacy }, budget, 20, 20, 20, 1).result,
    transport::policy_request_result_e::conflict);
  EXPECT_EQ(state.prepare_normalized_controller({ 42, 1, transport::control_source_e::legacy }, budget, 101, 20, 20, 1).result,
    transport::policy_request_result_e::invalid);
  ASSERT_EQ(state.prepare_normalized_controller({ 42, 1, transport::control_source_e::legacy }, budget, 20, 20, 20, 1).result,
    transport::policy_request_result_e::accepted);
  EXPECT_EQ(state.prepare_normalized_controller({ 42, 1, transport::control_source_e::legacy }, budget, 20, 20, 20, 2).result,
    transport::policy_request_result_e::conflict);
  state.stop();
  EXPECT_EQ(state.prepare_normalized_controller({ 42, 1, transport::control_source_e::legacy }, budget, 20, 20, 20, 2).result,
    transport::policy_request_result_e::stopped);
}

TEST(TransportPolicy, ManualBeforeAutomaticGrantWinsAndInvalidatesStartupLease) {
  transport::policy_state_t state(initial(), 50000);
  const auto prepared = state.prepare_normalized_controller({ 42, 1, transport::control_source_e::legacy }, initial().budget, 20, 20, 20, 1);
  ASSERT_EQ(prepared.result, transport::policy_request_result_e::accepted);
  const auto manual = state.request_normalized(initial().budget, 10, 30, 20, prepared.policy->revision, 1);
  ASSERT_EQ(manual.result, transport::policy_request_result_e::accepted);
  EXPECT_EQ(manual.policy->control_source, transport::control_source_e::manual);
  EXPECT_EQ(manual.policy->control_epoch, 2U);
  EXPECT_EQ(state.transfer_control({ 42, 1, transport::control_source_e::legacy }, transport::control_source_e::googcc,
                   manual.policy->revision)
              .result,
    transport::policy_request_result_e::conflict);
}

TEST(TransportPolicy, FailureKeepsPreviousEncoderAndProtection) {
  transport::policy_state_t state(initial(), 50000);
  ready(state);
  const auto old = state.active();
  const auto r = state.request_legacy_change(std::nullopt, 40);
  ASSERT_EQ(state.acquire_pending(), r.policy);
  EXPECT_TRUE(state.acknowledge_encoder(r.policy, transport::policy_failure_e::backend_failure));
  EXPECT_EQ(state.active(), old);
  EXPECT_FALSE(state.acquire_pending());
  EXPECT_FALSE(state.acknowledge_first_sent(r.policy, 2));
  EXPECT_EQ(state.snapshot().receipts.back().failure, transport::policy_failure_e::backend_failure);
}

TEST(TransportPolicy, NewCandidateDoesNotEraseInFlightApplication) {
  transport::policy_state_t state(initial(), 50000);
  ready(state);
  const auto first = state.request_legacy_change(35000, std::nullopt).policy;
  ASSERT_EQ(state.acquire_pending(), first);
  const auto second = state.request_legacy_change(std::nullopt, 30).policy;
  const auto latest = state.request_legacy_change(30000, std::nullopt).policy;
  const auto snapshot = state.snapshot();
  EXPECT_EQ(snapshot.receipts[2].failure, transport::policy_failure_e::superseded);
  EXPECT_TRUE(state.acknowledge_encoder(first, transport::policy_failure_e::none));
  EXPECT_EQ(state.active(), first);
  EXPECT_FALSE(state.acknowledge_encoder(second, transport::policy_failure_e::none));
  EXPECT_EQ(state.acquire_pending(), latest);
}

TEST(TransportPolicy, NormalizedBudgetUsesHighestFrameProtectionAndRejectsLegacyOverride) {
  transport::policy_state_t state(initial(), 50000);
  ready(state);
  transport::budget_request_t budget;
  budget.total_kbps = 40000;
  budget.other_kbps = 1000;
  budget.repair_kbps = 1000;
  budget.probe_kbps = 2000;
  budget.video_overhead_kbps = 1000;
  const auto r = state.request_normalized(budget, 10, 40, 20, 1, 1);
  ASSERT_EQ(r.result, transport::policy_request_result_e::accepted);
  EXPECT_EQ(r.policy->encoder_kbps, 25000);
  EXPECT_EQ(r.policy->fec_for_frame(true, true), 40u);
  EXPECT_EQ(r.policy->fec_for_frame(false, true), 20u);
  EXPECT_EQ(r.policy->fec_for_frame(false, false), 10u);
  EXPECT_EQ(state.request_legacy_change(30000, 10).result, transport::policy_request_result_e::conflict);
  EXPECT_EQ(state.request_normalized(budget, 10, 40, 20, 1, 1).result, transport::policy_request_result_e::conflict);
  EXPECT_EQ(r.policy->control_epoch, 2u);
  EXPECT_EQ(state.request_normalized(budget, 10, 40, 20, 2, 1).result, transport::policy_request_result_e::conflict);
}

TEST(TransportPolicy, InvalidAndForeignReceiptsDoNotChangeState) {
  transport::policy_state_t state(initial(), 50000);
  EXPECT_EQ(state.request_legacy_change(50001, 20).result, transport::policy_request_result_e::invalid);
  EXPECT_EQ(state.request_legacy_change(1, 80).result, transport::policy_request_result_e::invalid);
  EXPECT_EQ(state.request_legacy_change(30000, 101).result, transport::policy_request_result_e::invalid);
  ready(state);
  auto p = state.request_legacy_change(30000, 20).policy;
  ASSERT_EQ(state.acquire_pending(), p);
  auto forged = std::make_shared<const transport::frame_policy_t>(*p);
  EXPECT_FALSE(state.acknowledge_encoder(forged, transport::policy_failure_e::none));
  EXPECT_FALSE(state.acknowledge_encoder(p, static_cast<transport::policy_failure_e>(999)));
  EXPECT_TRUE(state.acknowledge_encoder(p, transport::policy_failure_e::unsupported));
  EXPECT_EQ(state.active()->revision, 1u);
}

TEST(TransportPolicy, ReinitializationKeepsConfirmedPolicyInsteadOfPendingTarget) {
  transport::policy_state_t state(initial(), 50000);
  ready(state);
  state.request_legacy_change(30000, 20);
  const auto p = state.begin_encoder_initialization();
  ASSERT_EQ(p->revision, 1u);
  EXPECT_FALSE(state.snapshot().encoder_initialized);
  EXPECT_FALSE(state.acquire_pending());
  EXPECT_TRUE(state.acknowledge_encoder(p, transport::policy_failure_e::backend_failure));
  EXPECT_TRUE(state.snapshot().receipts.front().encoder_applied);
  EXPECT_FALSE(state.snapshot().encoder_initialized);
  ready(state);
  EXPECT_EQ(state.acquire_pending()->revision, 2u);
}

TEST(TransportPolicy, StopCancelsPendingAndLateReceipts) {
  transport::policy_state_t state(initial(), 50000);
  ready(state);
  auto p = state.request_legacy_change(30000, 20).policy;
  ASSERT_EQ(state.acquire_pending(), p);
  state.stop();
  EXPECT_FALSE(state.acknowledge_encoder(p, transport::policy_failure_e::none));
  EXPECT_FALSE(state.acknowledge_first_sent(state.active(), 4));
  EXPECT_FALSE(state.begin_encoder_initialization());
  EXPECT_EQ(state.request_legacy_change(20000, 10).result, transport::policy_request_result_e::stopped);
  EXPECT_EQ(state.snapshot().receipts.back().failure, transport::policy_failure_e::stopped);
}

TEST(TransportPolicy, BoundedHistoryKeepsApplyingAndActiveVersions) {
  transport::policy_state_t state(initial(), 50000);
  ready(state);
  auto p = state.request_legacy_change(30000, 20).policy;
  ASSERT_EQ(state.acquire_pending(), p);
  for (int i = 0; i < 1000; ++i) ASSERT_EQ(state.request_legacy_change(30000 + i, 20).result, transport::policy_request_result_e::accepted);
  EXPECT_LE(state.snapshot().receipts.size(), transport::policy_state_t::receipt_capacity);
  EXPECT_TRUE(state.acknowledge_encoder(p, transport::policy_failure_e::none));
  EXPECT_TRUE(state.acknowledge_first_sent(p, 10));
}

TEST(TransportPolicy, BufferedOutputKeepsSubmissionPolicyAndNeverFallsBackToLatest) {
  transport::frame_policy_history_t history;
  auto a = std::make_shared<const transport::frame_policy_t>(initial());
  auto b = initial();
  b.revision = 2;
  b.fec_base = 40;
  auto latest = std::make_shared<const transport::frame_policy_t>(b);
  history.bind(100, a);
  history.bind(101, latest);
  EXPECT_EQ(history.find(100), a);
  EXPECT_EQ(history.find(101), latest);
  EXPECT_FALSE(history.find(102));
  history.bind(100 + transport::frame_policy_history_t::capacity, latest);
  EXPECT_FALSE(history.find(100));
}

TEST(TransportPolicy, SessionsAndConcurrentReadersStayIndependent) {
  transport::policy_state_t one(initial(42), 50000), two(initial(43), 50000);
  ready(one);
  ready(two);
  std::atomic<bool> done = false, consistent = true;
  std::thread reader([&] {
    while (!done.load()) {
      const auto s = one.snapshot();
      if (s.accepted->connection_epoch != 42 || s.applied->revision > s.accepted->revision ||
          s.receipts.size() > transport::policy_state_t::receipt_capacity) consistent = false;
    }
  });
  for (int i = 0; i < 1000; ++i) {
    auto r = one.request_legacy_change(30000 + i, 20);
    auto p = one.acquire_pending();
    if (r.result != transport::policy_request_result_e::accepted || p != r.policy ||
        !one.acknowledge_encoder(p, transport::policy_failure_e::none)) consistent = false;
  }
  done = true;
  reader.join();
  EXPECT_TRUE(consistent);
  EXPECT_EQ(two.active()->revision, 1u);
  EXPECT_EQ(two.active()->fec_base, 20u);
}

TEST(TransportPolicy, IdempotentRetryDoesNotApplyTwiceAndRejectsChangedContent) {
  transport::policy_state_t state(initial(), 50000);
  ready(state);
  transport::budget_request_t budget;
  budget.total_kbps = 40000;
  ASSERT_EQ(state.request_normalized(budget, 20, 20, 20, 1, 1, "takeover").result,
    transport::policy_request_result_e::accepted);
  auto first = state.request_normalized(budget, 20, 20, 20, 2, 2, "request-1");
  ASSERT_EQ(first.result, transport::policy_request_result_e::accepted);
  auto retry = state.request_normalized(budget, 20, 20, 20, 2, 2, "request-1");
  EXPECT_EQ(retry.policy, first.policy);
  EXPECT_EQ(state.snapshot().accepted->revision, 3u);
  budget.total_kbps = 30000;
  EXPECT_EQ(state.request_normalized(budget, 20, 20, 20, 2, 2, "request-1").result, transport::policy_request_result_e::conflict);
  EXPECT_EQ(state.snapshot().accepted->revision, 3u);
}

TEST(TransportPolicy, IdempotentRequestHistoryIsBoundedAndOldRetryCannotOverwrite) {
  transport::policy_state_t state(initial(), 50000);
  ready(state);
  transport::budget_request_t budget;
  budget.total_kbps = 40000;
  for (std::uint64_t revision = 1; revision <= 1000; ++revision) {
    ASSERT_EQ(state.request_normalized(budget, 20, 20, 20, revision, revision == 1 ? 1 : 2, std::to_string(revision)).result,
      transport::policy_request_result_e::accepted);
  }
  EXPECT_EQ(state.request_normalized(budget, 20, 20, 20, 1, 1, "1").result, transport::policy_request_result_e::conflict);
  EXPECT_EQ(state.snapshot().accepted->revision, 1001u);
}

namespace {
  transport::control_lease_t
  lease_of(const transport::frame_policy_ref_t &policy) {
    return { policy->connection_epoch, policy->control_epoch, policy->control_source };
  }
  transport::budget_request_t
  normalized_budget() {
    transport::budget_request_t budget;
    budget.total_kbps = 40000;
    budget.other_kbps = 1000;
    budget.repair_kbps = 1000;
    budget.probe_kbps = 2000;
    budget.video_overhead_kbps = 1000;
    return budget;
  }
}  // namespace

TEST(TransportPolicy, ManualTakeoverAdvancesEpochBeforeApplicationAndRejectsOldCachedRetry) {
  transport::policy_state_t state(initial(), 50000);
  const auto original = state.snapshot().accepted;
  const auto budget = normalized_budget();
  const auto takeover = state.request_normalized(budget, 10, 40, 20, 1, 1, "takeover");
  ASSERT_EQ(takeover.result, transport::policy_request_result_e::accepted);
  EXPECT_EQ(takeover.policy->control_source, transport::control_source_e::manual);
  EXPECT_EQ(takeover.policy->control_epoch, 2u);
  EXPECT_EQ(state.active(), original);
  EXPECT_EQ(original->control_source, transport::control_source_e::legacy);
  EXPECT_EQ(original->control_epoch, 1u);
  EXPECT_EQ(state.request_normalized(budget, 10, 40, 20, 1, 1, "takeover").result,
    transport::policy_request_result_e::conflict);
  const auto next = state.request_normalized(budget, 10, 40, 20, 2, 2, "manual-next");
  ASSERT_EQ(next.result, transport::policy_request_result_e::accepted);
  EXPECT_EQ(next.policy->control_epoch, 2u);
  EXPECT_EQ(state.request_normalized(budget, 10, 40, 20, 2, 2, "manual-next").policy, next.policy);
}

TEST(TransportPolicy, RejectedManualPolicyDoesNotTakeControlOrConsumeItsRequestId) {
  transport::policy_state_t state(initial(), 50000);
  const auto original = state.snapshot().accepted;
  auto budget = normalized_budget();
  budget.total_kbps = 1000;
  EXPECT_EQ(state.request_normalized(budget, 10, 40, 20, 1, 1, "manual").result,
    transport::policy_request_result_e::invalid);
  EXPECT_EQ(state.snapshot().accepted, original);
  budget = normalized_budget();
  EXPECT_EQ(state.request_normalized(budget, 10, 40, 20, 1, 1, "manual").result,
    transport::policy_request_result_e::accepted);
}

TEST(TransportPolicy, ControllerCannotAcquireFromMeasurementOrInventAConnectionLease) {
  transport::policy_state_t state(initial(), 50000);
  const auto original = state.snapshot().accepted;
  EXPECT_EQ(state.transfer_control(lease_of(original), transport::control_source_e::googcc, 1).result,
    transport::policy_request_result_e::invalid);
  auto forged = lease_of(original);
  forged.source = transport::control_source_e::googcc;
  EXPECT_EQ(state.request_controller_update(forged, normalized_budget(), 10, 40, 20, 1).result,
    transport::policy_request_result_e::conflict);
  EXPECT_EQ(state.snapshot().accepted, original);
}

TEST(TransportPolicy, HandoffChecksConnectionSourceEpochAndRevisionAtomically) {
  transport::policy_state_t state(initial(), 50000);
  const auto manual = state.request_normalized(normalized_budget(), 10, 40, 20, 1, 1).policy;
  ASSERT_TRUE(manual);
  const auto lease = lease_of(manual);
  auto invalid = lease;
  ++invalid.connection_epoch;
  EXPECT_EQ(state.transfer_control(invalid, transport::control_source_e::googcc, 2).result,
    transport::policy_request_result_e::conflict);
  invalid = lease;
  ++invalid.control_epoch;
  EXPECT_EQ(state.transfer_control(invalid, transport::control_source_e::googcc, 2).result,
    transport::policy_request_result_e::conflict);
  invalid = lease;
  invalid.source = transport::control_source_e::local;
  EXPECT_EQ(state.transfer_control(invalid, transport::control_source_e::googcc, 2).result,
    transport::policy_request_result_e::conflict);
  EXPECT_EQ(state.transfer_control(lease, transport::control_source_e::googcc, 1).result,
    transport::policy_request_result_e::conflict);
  for (const auto target : { transport::control_source_e::legacy, static_cast<transport::control_source_e>(999) })
    EXPECT_EQ(state.transfer_control(lease, target, 2).result, transport::policy_request_result_e::invalid);
  EXPECT_EQ(state.snapshot().accepted, manual);
}

TEST(TransportPolicy, HandoffPreservesBudgetProtectionAndHistoricalEncoderReceipt) {
  transport::policy_state_t state(initial(), 50000);
  ready(state);
  const auto manual = state.request_normalized(normalized_budget(), 10, 40, 20, 1, 1).policy;
  ASSERT_EQ(state.acquire_pending(), manual);
  ASSERT_TRUE(state.acknowledge_encoder(manual, transport::policy_failure_e::none));
  const auto handed = state.transfer_control(lease_of(manual), transport::control_source_e::googcc, 2);
  ASSERT_EQ(handed.result, transport::policy_request_result_e::accepted);
  EXPECT_EQ(handed.policy->revision, 3u);
  EXPECT_EQ(handed.policy->control_epoch, 3u);
  EXPECT_EQ(handed.policy->control_source, transport::control_source_e::googcc);
  EXPECT_EQ(handed.policy->budget, manual->budget);
  EXPECT_EQ(handed.policy->encoder_kbps, manual->encoder_kbps);
  EXPECT_EQ(handed.policy->fec_base, manual->fec_base);
  EXPECT_EQ(handed.policy->fec_key, manual->fec_key);
  EXPECT_EQ(handed.policy->fec_recovery, manual->fec_recovery);
  EXPECT_EQ(state.active(), manual);
  EXPECT_FALSE(state.snapshot().receipts.back().encoder_applied);
  EXPECT_EQ(state.request_legacy_change(45000, 0).result, transport::policy_request_result_e::conflict);
}

TEST(TransportPolicy, ManualTakeoverRejectsPreviouslySuccessfulControllerEvenOnCacheHit) {
  transport::policy_state_t state(initial(), 50000);
  const auto budget = normalized_budget();
  const auto manual = state.request_normalized(budget, 10, 40, 20, 1, 1).policy;
  const auto granted = state.transfer_control(lease_of(manual), transport::control_source_e::googcc, 2).policy;
  ASSERT_TRUE(granted);
  const auto lease = lease_of(granted);
  const auto automatic = state.request_controller_update(lease, budget, 5, 30, 10, 3, "gcc-1");
  ASSERT_EQ(automatic.result, transport::policy_request_result_e::accepted);
  EXPECT_EQ(state.request_controller_update(lease, budget, 5, 30, 10, 3, "gcc-1").policy, automatic.policy);
  const auto takeover = state.request_normalized(budget, 10, 40, 20, 4, 3, "manual-2");
  ASSERT_EQ(takeover.result, transport::policy_request_result_e::accepted);
  EXPECT_EQ(takeover.policy->control_epoch, 4u);
  EXPECT_EQ(state.request_controller_update(lease, budget, 5, 30, 10, 3, "gcc-1").result,
    transport::policy_request_result_e::conflict);
  EXPECT_EQ(state.request_controller_update(lease, budget, 5, 30, 10, 5, "gcc-2").result,
    transport::policy_request_result_e::conflict);
  EXPECT_EQ(state.snapshot().accepted, takeover.policy);
}

TEST(TransportPolicy, CorrectEpochDoesNotPermitADifferentControllerSourceOrConnection) {
  transport::policy_state_t state(initial(), 50000);
  const auto manual = state.request_normalized(normalized_budget(), 10, 40, 20, 1, 1).policy;
  const auto granted = state.transfer_control(lease_of(manual), transport::control_source_e::googcc, 2).policy;
  auto forged = lease_of(granted);
  forged.source = transport::control_source_e::local;
  EXPECT_EQ(state.request_controller_update(forged, normalized_budget(), 10, 40, 20, 3).result,
    transport::policy_request_result_e::conflict);
  forged = lease_of(granted);
  ++forged.connection_epoch;
  EXPECT_EQ(state.request_controller_update(forged, normalized_budget(), 10, 40, 20, 3).result,
    transport::policy_request_result_e::conflict);
  forged = lease_of(granted);
  forged.source = static_cast<transport::control_source_e>(999);
  EXPECT_EQ(state.request_controller_update(forged, normalized_budget(), 10, 40, 20, 3).result,
    transport::policy_request_result_e::invalid);
  EXPECT_EQ(state.snapshot().accepted, granted);
}

TEST(TransportPolicy, ReplacingSameControllerSourceAlwaysInvalidatesOldInstance) {
  transport::policy_state_t state(initial(), 50000);
  const auto manual = state.request_normalized(normalized_budget(), 10, 40, 20, 1, 1).policy;
  const auto first = state.transfer_control(lease_of(manual), transport::control_source_e::googcc, 2).policy;
  const auto second = state.transfer_control(lease_of(first), transport::control_source_e::googcc, 3).policy;
  ASSERT_TRUE(second);
  EXPECT_EQ(second->control_epoch, 4u);
  EXPECT_EQ(state.request_controller_update(lease_of(first), normalized_budget(), 10, 40, 20, 4).result,
    transport::policy_request_result_e::conflict);
  EXPECT_EQ(state.request_controller_update(lease_of(second), normalized_budget(), 10, 40, 20, 4).result,
    transport::policy_request_result_e::accepted);
}

TEST(TransportPolicy, InFlightEncoderCanReportTruthfullyWithoutReacquiringControl) {
  transport::policy_state_t state(initial(), 50000);
  ready(state);
  const auto manual = state.request_normalized(normalized_budget(), 10, 40, 20, 1, 1).policy;
  const auto granted = state.transfer_control(lease_of(manual), transport::control_source_e::googcc, 2).policy;
  ASSERT_EQ(state.acquire_pending(), granted);
  const auto takeover = state.request_normalized(normalized_budget(), 15, 40, 25, 3, 3).policy;
  ASSERT_TRUE(takeover);
  ASSERT_TRUE(state.acknowledge_encoder(granted, transport::policy_failure_e::none));
  EXPECT_EQ(state.active(), granted);
  EXPECT_EQ(state.snapshot().accepted, takeover);
  EXPECT_EQ(state.snapshot().accepted->control_source, transport::control_source_e::manual);
  EXPECT_EQ(state.request_controller_update(lease_of(granted), normalized_budget(), 10, 40, 20, 4).result,
    transport::policy_request_result_e::conflict);
  EXPECT_EQ(state.acquire_pending(), takeover);
}

TEST(TransportPolicy, StoppedSessionCannotGrantOrUseAControllerLease) {
  transport::policy_state_t state(initial(), 50000);
  const auto manual = state.request_normalized(normalized_budget(), 10, 40, 20, 1, 1).policy;
  const auto granted = state.transfer_control(lease_of(manual), transport::control_source_e::local, 2).policy;
  ASSERT_TRUE(granted);
  state.stop();
  EXPECT_EQ(state.transfer_control(lease_of(granted), transport::control_source_e::manual, 3).result,
    transport::policy_request_result_e::stopped);
  EXPECT_EQ(state.request_controller_update(lease_of(granted), normalized_budget(), 10, 40, 20, 3).result,
    transport::policy_request_result_e::stopped);
}

TEST(TransportPolicy, ManualAndAutomaticConcurrentCandidatesHaveExactlyOneWinner) {
  for (unsigned iteration = 0; iteration < 100; ++iteration) {
    transport::policy_state_t state(initial(), 50000);
    const auto budget = normalized_budget();
    const auto manual = state.request_normalized(budget, 10, 40, 20, 1, 1).policy;
    const auto granted = state.transfer_control(lease_of(manual), transport::control_source_e::googcc, 2).policy;
    std::atomic<bool> start = false;
    transport::policy_request_result_t automatic, requested;
    std::thread controller([&] {
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      automatic = state.request_controller_update(lease_of(granted), budget, 5, 30, 10, 3, "automatic");
    });
    std::thread user([&] {
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      requested = state.request_normalized(budget, 15, 40, 25, 3, 3, "manual");
    });
    start.store(true, std::memory_order_release);
    controller.join();
    user.join();
    const bool manual_won = requested.result == transport::policy_request_result_e::accepted;
    const bool controller_won = automatic.result == transport::policy_request_result_e::accepted;
    EXPECT_NE(manual_won, controller_won);
    const auto accepted = state.snapshot().accepted;
    EXPECT_EQ(accepted->revision, 4u);
    EXPECT_EQ(accepted->control_epoch, manual_won ? 4u : 3u);
    EXPECT_EQ(accepted->control_source, manual_won ? transport::control_source_e::manual : transport::control_source_e::googcc);
  }
}
