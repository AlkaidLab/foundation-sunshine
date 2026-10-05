#include "src/transport/transport_budget.h"
#include "src/transport/transport_send_budget.h"

#include <chrono>
#include <future>
#include <limits>
#include <random>
#include <stdexcept>

#include <gtest/gtest.h>

namespace {
  using namespace transport;
  constexpr std::int64_t scale = 1000000;
  constexpr std::uint64_t epoch = 7;
  constexpr auto accepted = send_budget_result_e::accepted;
  constexpr auto video = send_traffic_e::video;

  TEST(SessionSendBudget, PolicyRevisionRejectsCrossOutletRollbackAndConflictingSameRevision) {
    session_send_budget_t budget(epoch, { 1000, 200, 100 }, 0, 4);
    const auto newer = budget.try_update(epoch, { 500, 100, 0 }, 1000, 6);
    ASSERT_EQ(newer.result, accepted);
    ASSERT_TRUE(newer.event);
    EXPECT_EQ(newer.event->policy_revision, 6U);
    EXPECT_EQ(budget.try_update(epoch, { 1000, 200, 100 }, 2000, 4).result, send_budget_result_e::stale_revision);
    EXPECT_EQ(budget.try_update(epoch, { 1000, 200, 100 }, 2000, 0).result, send_budget_result_e::stale_revision);
    EXPECT_EQ(budget.try_update(epoch, { 1000, 200, 100 }, 2000, 6).result, send_budget_result_e::invalid);
    const auto current = *budget.try_snapshot();
    EXPECT_EQ(current.limits.rate_bytes_per_second, 500U);
    EXPECT_EQ(current.ordinal, newer.event->ordinal);
    EXPECT_EQ(current.at_us, newer.event->at_us);
    EXPECT_EQ(current.credit_microbytes, newer.event->credit_microbytes);
  }

  TEST(SessionSendBudget, SameLimitsNewRevisionHasAuditableBoundaryWithoutMintingCredit) {
    session_send_budget_t budget(epoch, { 1000, 100, 0 }, 0, 1);
    auto send = budget.try_reserve(epoch, video, 100, 100, 0);
    ASSERT_TRUE(send.permit->begin_submission());
    ASSERT_TRUE(send.permit->complete(100, 1, true, 0).accounting_valid);
    const auto update = budget.try_update(epoch, { 1000, 100, 0 }, 1000, 2);
    ASSERT_TRUE(update.event);
    EXPECT_EQ(update.event->credit_microbytes, scale);
    EXPECT_EQ(update.event->policy_revision, 2U);
    EXPECT_EQ(update.event->ordinal, 2U);
    EXPECT_FALSE(budget.try_update(epoch, { 1000, 100, 0 }, 2000, 2).event);
    auto audio = budget.try_reserve(epoch, send_traffic_e::audio, 100, 28, 28000);
    ASSERT_TRUE(audio.permit);
    ASSERT_TRUE(audio.permit->begin_submission());
    EXPECT_EQ(audio.permit->complete(28, 1, true, 28000).policy_revision, 2U);
  }

  TEST(SessionSendBudget, SharesActualIPv4IPv6BytesAcrossAllTrafficClasses) {
    session_send_budget_t budget(epoch, { 1000000, 10000, 1000 }, 0);
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < static_cast<std::size_t>(send_traffic_e::count); ++i) {
      const auto bytes = *ip_datagram_bytes(100 + i * 100, i % 2);
      auto reservation = budget.try_reserve(epoch, static_cast<send_traffic_e>(i), bytes, bytes, 0);
      ASSERT_EQ(reservation.result, accepted);
      ASSERT_TRUE(reservation.permit->begin_submission());
      const auto receipt = reservation.permit->complete(bytes, 1, true, 0);
      EXPECT_TRUE(receipt.accounting_valid);
      EXPECT_EQ(receipt.ordinal, i + 1);
      EXPECT_EQ(receipt.successful_ip_bytes, bytes);
      total += bytes;
    }
    const auto snapshot = *budget.try_snapshot();
    EXPECT_EQ(snapshot.credit_microbytes, (10000 - static_cast<std::int64_t>(total)) * scale);
    for (std::size_t i = 0; i < snapshot.successful_ip_bytes.size(); ++i) {
      EXPECT_EQ(snapshot.successful_ip_bytes[i], *ip_datagram_bytes(100 + i * 100, i % 2));
      EXPECT_EQ(snapshot.successful_packets[i], 1U);
    }
  }

  TEST(SessionSendBudget, ZeroFailureAndPartialSuccessOnlyChargeKnownSubmittedBytes) {
    session_send_budget_t budget(epoch, { 1000, 200, 100 }, 0);
    auto failed = budget.try_reserve(epoch, video, 300, 300, 0);
    ASSERT_EQ(failed.result, accepted);
    ASSERT_TRUE(failed.permit->begin_submission());
    const auto zero = failed.permit->complete(0, 0, true, 0);
    EXPECT_EQ(zero.credit_microbytes, 200 * scale);
    auto partial = budget.try_reserve(epoch, video, 300, 300, 0);
    ASSERT_EQ(partial.result, accepted);
    ASSERT_TRUE(partial.permit->begin_submission());
    const auto receipt = partial.permit->complete(100, 1, true, 0);
    EXPECT_EQ(receipt.credit_microbytes, 100 * scale);
    EXPECT_EQ(receipt.successful_ip_bytes, 100U);
    EXPECT_EQ(receipt.uncertain_ip_bytes, 0U);
  }

  TEST(SessionSendBudget, DelayedCompletionCannotMintRefundOrPrechargeCredit) {
    session_send_budget_t budget(epoch, { 1000, 200, 0 }, 0);
    auto permit = budget.try_reserve(epoch, video, 200, 200, 0);
    ASSERT_EQ(permit.result, accepted);
    ASSERT_TRUE(permit.permit->begin_submission());
    const auto receipt = permit.permit->complete(100, 1, true, 100000);
    EXPECT_EQ(receipt.credit_microbytes, 100 * scale);
    EXPECT_EQ(budget.try_reserve(epoch, video, 200, 200, 100000).result, send_budget_result_e::insufficient);
    auto later = budget.try_reserve(epoch, video, 200, 200, 200000);
    ASSERT_EQ(later.result, accepted);
    ASSERT_TRUE(later.permit->begin_submission());
    EXPECT_EQ(later.permit->complete(200, 2, true, 200000).credit_microbytes, 0);
  }

  TEST(SessionSendBudget, DebtAndOldRateIntegralSurviveLimitReduction) {
    session_send_budget_t budget(epoch, { 1000, 100, 100 }, 0);
    auto permit = budget.try_reserve(epoch, video, 200, 200, 0);
    ASSERT_EQ(permit.result, accepted);
    ASSERT_TRUE(permit.permit->begin_submission());
    EXPECT_EQ(permit.permit->complete(200, 2, true, 0).credit_microbytes, -100 * scale);
    const auto update = budget.try_update(epoch, { 100, 50, 0 }, 10000);
    ASSERT_EQ(update.result, accepted);
    ASSERT_TRUE(update.event);
    EXPECT_EQ(update.event->credit_microbytes, -90 * scale);
    EXPECT_EQ(budget.try_reserve(epoch, video, 28, 28, 910000).result, send_budget_result_e::insufficient);
    auto resumed = budget.try_reserve(epoch, video, 28, 28, 1190000);
    ASSERT_EQ(resumed.result, accepted);
    ASSERT_TRUE(resumed.permit->begin_submission());
    EXPECT_EQ(resumed.permit->complete(28, 1, true, 1190000).credit_microbytes, 0);
    const auto raised = budget.try_update(epoch, { 10000, 1000, 0 }, 1190000);
    ASSERT_TRUE(raised.event);
    EXPECT_EQ(raised.event->credit_microbytes, 0);
  }

  TEST(SessionSendBudget, RateChangesIntegrateEachRateAtItsOwnBoundaryAndCapCredit) {
    session_send_budget_t budget(epoch, { 1000, 1000, 0 }, 0);
    auto initial = budget.try_reserve(epoch, video, 1000, 1000, 0);
    ASSERT_EQ(initial.result, accepted);
    ASSERT_TRUE(initial.permit->begin_submission());
    EXPECT_EQ(initial.permit->complete(1000, 1, true, 0).credit_microbytes, 0);
    const auto update = budget.try_update(epoch, { 2000, 1000, 0 }, 250000);
    ASSERT_EQ(update.result, accepted);
    ASSERT_TRUE(update.event);
    EXPECT_EQ(update.event->credit_microbytes, 250 * scale);
    EXPECT_EQ(budget.try_reserve(epoch, video, 251, 251, 250000).result, send_budget_result_e::insufficient);
    auto old_rate = budget.try_reserve(epoch, video, 250, 250, 250000);
    ASSERT_EQ(old_rate.result, accepted);
    EXPECT_TRUE(old_rate.permit->cancel_before_send(250000).accounting_valid);
    EXPECT_EQ(budget.try_reserve(epoch, video, 751, 751, 500000).result, send_budget_result_e::insufficient);
    auto both_rates = budget.try_reserve(epoch, video, 750, 750, 500000);
    ASSERT_EQ(both_rates.result, accepted);
    EXPECT_EQ(both_rates.permit->cancel_before_send(500000).credit_microbytes, 750 * scale);
    const auto cap = budget.try_update(epoch, { 2000, 100, 0 }, 500000);
    ASSERT_EQ(cap.result, accepted);
    ASSERT_TRUE(cap.event);
    EXPECT_EQ(cap.event->credit_microbytes, 100 * scale);
    EXPECT_EQ(budget.try_reserve(epoch, video, 101, 101, 500000).result, send_budget_result_e::insufficient);
  }

  TEST(SessionSendBudget, UpToReservationClipsAtAvailableWholeIPBytes) {
    session_send_budget_t budget(epoch, { 1000, 100, 20 }, 0);
    auto permit = budget.try_reserve(epoch, send_traffic_e::audio, 1000, 48, 0);
    ASSERT_EQ(permit.result, accepted);
    ASSERT_TRUE(permit.permit->begin_submission());
    EXPECT_EQ(permit.permit->ip_bytes(), 120U);
    EXPECT_EQ(permit.permit->complete(100, 1, true, 0).credit_microbytes, 0);
    EXPECT_EQ(budget.try_reserve(epoch, video, 100, 28, 0).result, send_budget_result_e::insufficient);
    auto fraction = budget.try_reserve(epoch, send_traffic_e::control, 100, 28, 8000);
    ASSERT_EQ(fraction.result, accepted);
    ASSERT_TRUE(fraction.permit->begin_submission());
    EXPECT_EQ(fraction.permit->ip_bytes(), 28U);
    EXPECT_EQ(fraction.permit->complete(28, 1, true, 8000).credit_microbytes, -20 * scale);
  }

  TEST(SessionSendBudget, UnknownSuffixPreservesSuccessPrefixAndClosesOnlyItsEpoch) {
    session_send_budget_t budget(epoch, { 1000000, 10000, 1000 }, 0);
    auto permit = budget.try_reserve(epoch, video, 4308, 4308, 0);
    ASSERT_EQ(permit.result, accepted);
    ASSERT_TRUE(permit.permit->begin_submission());
    const auto receipt = permit.permit->complete(1436, 1, false, 0);
    EXPECT_EQ(receipt.successful_ip_bytes, 1436U);
    EXPECT_EQ(receipt.uncertain_ip_bytes, 2872U);
    EXPECT_FALSE(receipt.accounting_valid);
    EXPECT_TRUE(receipt.stopped);
    const auto state = *budget.try_snapshot();
    EXPECT_EQ(state.successful_ip_bytes[0], 1436U);
    EXPECT_EQ(state.uncertain_ip_bytes, 2872U);
    EXPECT_EQ(budget.try_reserve(epoch, video, 28, 28, 100000).result, send_budget_result_e::stopped);
    session_send_budget_t replacement(8, { 1000000, 10000, 1000 }, 0);
    EXPECT_FALSE(replacement.stop(epoch));
    EXPECT_TRUE(replacement.try_snapshot()->accounting_valid);
  }

  TEST(SessionSendBudget, AbandonedPermitIsUncertainButExplicitPreSendCancelIsSafe) {
    session_send_budget_t budget(epoch, { 1000, 200, 0 }, 0);
    {
      auto permit = budget.try_reserve(epoch, video, 100, 100, 0);
      ASSERT_EQ(permit.result, accepted);
    }
    EXPECT_FALSE(budget.try_snapshot()->accounting_valid);
    EXPECT_EQ(budget.try_snapshot()->uncertain_ip_bytes, 100U);
    session_send_budget_t safe(8, { 1000, 200, 0 }, 0);
    auto permit = safe.try_reserve(8, video, 100, 100, 0);
    ASSERT_EQ(permit.result, accepted);
    const auto receipt = permit.permit->cancel_before_send(1000);
    EXPECT_TRUE(receipt.accounting_valid);
    EXPECT_EQ(receipt.credit_microbytes, 200 * scale);
    EXPECT_EQ(receipt.successful_packets, 0U);
    EXPECT_FALSE(permit.permit->complete(100, 1, true, 1000).accounting_valid);
    EXPECT_TRUE(safe.try_snapshot()->accounting_valid);
  }

  TEST(SessionSendBudget, CrossThreadAdmissionAndUpdatesNeverWaitForLivePermit) {
    session_send_budget_t budget(epoch, { 1000, 200, 0 }, 0);
    auto permit = budget.try_reserve(epoch, video, 100, 100, 0);
    ASSERT_EQ(permit.result, accepted);
    auto child = std::async(std::launch::async, [&] {
      return budget.try_reserve(epoch, send_traffic_e::audio, 28, 28, 0).result == send_budget_result_e::busy &&
             budget.try_update(epoch, { 1000, 100, 0 }, 0).result == send_budget_result_e::busy &&
             !budget.try_snapshot();
    });
    const auto ready = child.wait_for(std::chrono::seconds(1));
    EXPECT_EQ(ready, std::future_status::ready);
    if (ready != std::future_status::ready) permit.permit->cancel_before_send(0);
    EXPECT_TRUE(child.get());
    if (ready == std::future_status::ready) {
      EXPECT_TRUE(permit.permit->cancel_before_send(0).accounting_valid);
    }
  }

  TEST(SessionSendBudget, StopIsImmediateAndKnownInFlightSuccessStillSettles) {
    session_send_budget_t budget(epoch, { 1000, 200, 0 }, 0);
    auto permit = budget.try_reserve(epoch, video, 100, 100, 0);
    ASSERT_EQ(permit.result, accepted);
    ASSERT_TRUE(permit.permit->begin_submission());
    auto child = std::async(std::launch::async, [&] { return budget.stop(epoch); });
    EXPECT_TRUE(child.get());
    EXPECT_FALSE(permit.permit->allowed_to_send());
    EXPECT_EQ(budget.try_update(epoch, { 1000, 100, 0 }, 0).result, send_budget_result_e::stopped);
    const auto receipt = permit.permit->complete(100, 1, true, 100);
    EXPECT_TRUE(receipt.accounting_valid);
    EXPECT_TRUE(receipt.stopped);
    EXPECT_EQ(receipt.successful_ip_bytes, 100U);
    EXPECT_EQ(budget.try_reserve(epoch, video, 28, 28, 1000).result, send_budget_result_e::stopped);
  }

  TEST(SessionSendBudget, OwnerLifetimeAndPermitMovesRetainOneSettlement) {
    auto owner = std::make_unique<session_send_budget_t>(epoch, send_budget_limits_t { 1000, 200, 0 }, 0);
    auto reservation = owner->try_reserve(epoch, video, 100, 100, 0);
    ASSERT_EQ(reservation.result, accepted);
    auto moved = std::move(*reservation.permit);
    EXPECT_EQ(reservation.permit->ip_bytes(), 0U);
    ASSERT_TRUE(moved.begin_submission());
    owner.reset();
    EXPECT_FALSE(moved.allowed_to_send());
    const auto receipt = moved.complete(100, 1, true, 0);
    EXPECT_EQ(receipt.successful_packets, 1U);
    EXPECT_TRUE(receipt.accounting_valid);
  }

  TEST(SessionSendBudget, InvalidInputsAndContradictoryCompletionCannotCreateCredit) {
    const send_budget_limits_t valid { 1000, 200, 0 };
    EXPECT_THROW(session_send_budget_t(0, valid, 0), std::invalid_argument);
    EXPECT_THROW(session_send_budget_t(epoch, valid, -1), std::invalid_argument);
    const send_budget_limits_t invalid { 0, 200, 0 };
    EXPECT_THROW(session_send_budget_t(epoch, invalid, 0), std::invalid_argument);
    session_send_budget_t budget(epoch, valid, 0);
    EXPECT_EQ(budget.try_reserve(8, video, 28, 28, 0).result, send_budget_result_e::foreign_epoch);
    EXPECT_EQ(budget.try_reserve(epoch, send_traffic_e::count, 28, 28, 0).result, send_budget_result_e::invalid);
    EXPECT_EQ(budget.try_reserve(epoch, video, 28, 1, 0).result, send_budget_result_e::invalid);
    EXPECT_EQ(budget.try_reserve(epoch, video, std::numeric_limits<std::uint64_t>::max(), 28, 0).result, send_budget_result_e::invalid);
    auto permit = budget.try_reserve(epoch, video, 100, 100, 0);
    ASSERT_EQ(permit.result, accepted);
    ASSERT_TRUE(permit.permit->begin_submission());
    const auto receipt = permit.permit->complete(101, 1, true, 0);
    EXPECT_FALSE(receipt.accounting_valid);
    EXPECT_EQ(receipt.successful_ip_bytes, 0U);
    EXPECT_EQ(receipt.uncertain_ip_bytes, 100U);
    session_send_budget_t unstarted(8, valid, 0);
    auto invalid_success = unstarted.try_reserve(8, video, 100, 100, 0);
    ASSERT_EQ(invalid_success.result, accepted);
    EXPECT_FALSE(invalid_success.permit->complete(100, 1, true, 0).accounting_valid);
  }

  TEST(SessionSendBudget, RandomMixedTrafficMatchesIndependentIntegerCreditOracle) {
    send_budget_limits_t limits { 50000, 2000, 500 };
    session_send_budget_t budget(epoch, limits, 0);
    std::mt19937 random(20261003);
    std::int64_t now = 0, prior = 0, credit = 2000 * scale;
    std::array<std::uint64_t, 5> bytes {}, packets {};
    const auto advance = [&](std::int64_t at) {
      credit = std::min(static_cast<std::int64_t>(limits.burst_bytes * scale), credit + static_cast<std::int64_t>(limits.rate_bytes_per_second) * (at - prior));
      prior = at;
    };
    for (unsigned i = 0; i < 1000; ++i) {
      now += random() % 5000;
      if (i % 11 == 0) {
        advance(now);
        limits = { 1000 + random() % 100000, 100 + random() % 2000, random() % 1000 };
        const auto update = budget.try_update(epoch, limits, now);
        ASSERT_EQ(update.result, accepted);
        credit = std::min(credit, static_cast<std::int64_t>(limits.burst_bytes * scale));
        ASSERT_TRUE(update.event);
        EXPECT_EQ(update.event->credit_microbytes, credit);
      }
      const auto kind = random() % 5;
      advance(now);
      auto reservation = budget.try_reserve(epoch, static_cast<send_traffic_e>(kind), 100 + random() % 3000, 28, now);
      if (reservation.result == send_budget_result_e::insufficient) continue;
      ASSERT_EQ(reservation.result, accepted);
      const auto actual_packets = (reservation.permit->ip_bytes() / 28) / (1 + random() % 3);
      const auto actual_bytes = actual_packets * 28;
      ASSERT_TRUE(reservation.permit->begin_submission());
      now += random() % 30000;
      advance(now);
      credit -= static_cast<std::int64_t>(actual_bytes * scale);
      const auto receipt = reservation.permit->complete(actual_bytes, actual_packets, true, now);
      EXPECT_EQ(receipt.credit_microbytes, credit);
      EXPECT_TRUE(receipt.accounting_valid);
      bytes[kind] += actual_bytes;
      packets[kind] += actual_packets;
    }
    const auto state = *budget.try_snapshot();
    EXPECT_EQ(state.successful_ip_bytes, bytes);
    EXPECT_EQ(state.successful_packets, packets);
    EXPECT_EQ(state.credit_microbytes, credit);
  }

  TEST(SessionSendBudget, CancelAfterSubmissionBeginsCannotRefundUncertainBytes) {
    session_send_budget_t budget(epoch, { 1000, 200, 0 }, 0);
    auto before = budget.try_reserve(epoch, video, 100, 100, 0);
    ASSERT_EQ(before.result, accepted);
    EXPECT_TRUE(before.permit->cancel_before_send(0).accounting_valid);
    auto after = budget.try_reserve(epoch, video, 100, 100, 0);
    ASSERT_EQ(after.result, accepted);
    ASSERT_TRUE(after.permit->begin_submission());
    EXPECT_FALSE(after.permit->begin_submission());
    const auto receipt = after.permit->cancel_before_send(0);
    EXPECT_FALSE(receipt.accounting_valid);
    EXPECT_EQ(receipt.uncertain_ip_bytes, 100U);
    EXPECT_EQ(receipt.credit_microbytes, 100 * scale);
  }

  TEST(SessionSendBudget, CancellingWithOlderSamplePreservesKnownZeroAndSubmissionUncertainty) {
    session_send_budget_t budget(1, {1000, 1000, 0}, 100);
    auto before = budget.try_reserve(1, send_traffic_e::video, 100, 100, 90);
    ASSERT_TRUE(before.permit);
    const auto cancelled = before.permit->cancel_before_send(90);
    EXPECT_TRUE(cancelled.accounting_valid);
    EXPECT_TRUE(cancelled.completion_known);
    EXPECT_EQ(cancelled.reserved_at_us, 100);
    EXPECT_EQ(cancelled.at_us, 100);
    EXPECT_EQ(cancelled.uncertain_ip_bytes, 0U);
    EXPECT_EQ(cancelled.credit_microbytes, 1000 * scale);

    auto after = budget.try_reserve(1, send_traffic_e::audio, 100, 100, 90);
    ASSERT_TRUE(after.permit);
    ASSERT_TRUE(after.permit->begin_submission());
    const auto uncertain = after.permit->cancel_before_send(90);
    EXPECT_FALSE(uncertain.accounting_valid);
    EXPECT_FALSE(uncertain.completion_known);
    EXPECT_EQ(uncertain.uncertain_ip_bytes, 100U);
    EXPECT_TRUE(budget.try_snapshot()->stopped);
  }

  TEST(SessionSendBudget, MaximumClockAndCreditDoNotOverflowRefillArithmetic) {
    constexpr auto maximum_time = std::numeric_limits<std::int64_t>::max() / 4;
    session_send_budget_t budget(epoch, { 100000000, 1000000000, 0 }, 0);
    auto permit = budget.try_reserve(epoch, video, 1000000000, 28, 0);
    ASSERT_EQ(permit.result, accepted);
    ASSERT_TRUE(permit.permit->begin_submission());
    const auto receipt = permit.permit->complete(28, 1, true, maximum_time);
    EXPECT_EQ(receipt.credit_microbytes, 999999972LL * scale);
    EXPECT_TRUE(receipt.accounting_valid);
    EXPECT_EQ(budget.try_reserve(epoch, video, 28, 28, maximum_time + 1).result, send_budget_result_e::invalid);
    EXPECT_EQ(budget.try_update(epoch, { 100000001, 100, 0 }, maximum_time).result, send_budget_result_e::invalid);
    EXPECT_EQ(budget.try_update(epoch, { 1000, 1000000000, 1 }, maximum_time).result, send_budget_result_e::invalid);
  }

  TEST(SessionSendBudget, ConcurrentRealClockSendersKeepOneAuthoritativeReceiptOrdinal) {
    const auto clock = [] { return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
    session_send_budget_t budget(epoch, { 100000000, 1000000, 0 }, clock());
    std::array<std::future<std::uint64_t>, 4> workers;
    for (std::size_t kind = 0; kind < workers.size(); ++kind) {
      workers[kind] = std::async(std::launch::async, [&, kind] {
        std::uint64_t count = 0;
        for (unsigned i = 0; i < 2000; ++i) {
          auto permit = budget.try_reserve(epoch, static_cast<send_traffic_e>(kind), 28, 28, clock());
          if (permit.result == send_budget_result_e::busy) continue;
          EXPECT_EQ(permit.result, accepted);
          if (!permit.permit) break;
          EXPECT_TRUE(permit.permit->begin_submission());
          const auto receipt = permit.permit->complete(28, 1, true, clock());
          EXPECT_TRUE(receipt.accounting_valid);
          ++count;
        }
        return count;
      });
    }
    std::array<std::uint64_t, 4> counts;
    for (std::size_t i = 0; i < workers.size(); ++i) counts[i] = workers[i].get();
    const auto state = *budget.try_snapshot();
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < counts.size(); ++i) {
      EXPECT_EQ(state.successful_packets[i], counts[i]);
      EXPECT_EQ(state.successful_ip_bytes[i], counts[i] * 28);
      total += counts[i];
    }
    EXPECT_GT(total, 0U);
    EXPECT_EQ(state.ordinal, total);
    EXPECT_TRUE(state.accounting_valid);
    EXPECT_FALSE(state.stopped);
  }
}  // namespace
