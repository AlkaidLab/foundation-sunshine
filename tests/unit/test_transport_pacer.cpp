#include "src/transport/transport_pacer.h"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

namespace {
  using namespace transport;

  pacing_limits_t
  limits(std::uint64_t rate = 1000000, std::uint64_t burst = 2400, std::uint64_t debt = 0) {
    return { { rate, burst, debt }, { rate, burst, debt } };
  }

  frame_policy_ref_t
  policy(std::uint64_t epoch = 7, std::uint64_t revision = 1) {
    frame_policy_t value;
    value.connection_epoch = epoch;
    value.revision = revision;
    value.fec_base = 20;
    return std::make_shared<const frame_policy_t>(value);
  }

  paced_frame_t
  frame(std::uint64_t id, std::uint64_t first_sequence, std::size_t packets,
    std::uint32_t ip_bytes = 1200, std::int64_t deadline = 1000000,
    frame_dependency_e dependency = frame_dependency_e::non_reference,
    frame_policy_ref_t snapshot = policy(), bool ipv6 = false) {
    paced_frame_t value;
    value.frame_id = id;
    value.policy = std::move(snapshot);
    value.deadline_us = deadline;
    value.dependency = dependency;
    for (std::size_t i = 0; i < packets; ++i) {
      owned_paced_packet_t packet;
      packet.ipv6 = ipv6;
      packet.udp_payload.assign(ip_bytes - (ipv6 ? 48 : 28), static_cast<std::uint8_t>(first_sequence + i));
      packet.metadata = { first_sequence + i, 0, ip_bytes, id, value.policy->revision,
        i % 2 == 0 ? packet_kind_e::data : packet_kind_e::fec, {} };
      value.packets.push_back(std::move(packet));
    }
    return value;
  }

  deadline_pacer_t::sender_t
  all_success(std::int64_t now, std::vector<sent_packet_t> *trace = nullptr) {
    return [now, trace](std::uint64_t, std::span<const paced_packet_view_t> packets) {
      paced_batch_submission_t result;
      result.completed_at_us = now;
      for (const auto &packet : packets) {
        result.packets.push_back({ true, now });
        if (trace) {
          auto record = packet.metadata;
          record.send_time_us = now;
          trace->push_back(record);
        }
      }
      return result;
    };
  }

  paced_frame_t
  primary_then_fec(std::uint64_t id, std::uint64_t sequence, std::size_t data, std::size_t fec,
    std::int64_t deadline, frame_dependency_e dependency) {
    auto value = frame(id, sequence, data + fec, 100, deadline, dependency);
    for (std::size_t i = 0; i < value.packets.size(); ++i)
      value.packets[i].metadata.kind = i < data ? packet_kind_e::data : packet_kind_e::fec;
    return value;
  }

  TEST(TransportPacer, RealMediaProbeGroupsPreserveIdentityPayloadAndIntegralBudget) {
    deadline_pacer_t pacer;
    const auto handle = *pacer.add_session(7, limits(1000000, 20000), 0);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(1, 100, 10), 0).result, pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.start_probe(handle, { 9, 8000, 6000, 2000, 3 }, 0), paced_probe_result_e::active);
    std::set<std::uint64_t> identities;
    for (const auto now : { 0, 2400, 4800 }) {
      const auto sent = pacer.dispatch(now, [&](auto, auto packets) {
        EXPECT_EQ(packets.size(), 2U);
        for (const auto &packet : packets) {
          EXPECT_EQ(packet.metadata.probe.cluster_id, 9);
          EXPECT_EQ(packet.metadata.probe.min_bytes, 6000);
          EXPECT_EQ(packet.metadata.probe.min_packets, 3);
          EXPECT_EQ(packet.udp_payload[0], static_cast<std::uint8_t>(packet.metadata.extended_sequence));
        }
        return all_success(now)(handle, packets);
      });
      ASSERT_EQ(sent.successful.size(), 2U);
      for (const auto &success : sent.successful) {
        EXPECT_TRUE(identities.insert(success.packet.extended_sequence).second);
        EXPECT_EQ(success.packet.probe.cluster_id, 9);
        EXPECT_EQ(success.packet.frame_id, 1U);
      }
      if (now < 4800) {
        EXPECT_FALSE(pacer.dispatch(now + 1, all_success(now + 1)).attempted);
        EXPECT_EQ(*sent.next_wakeup_us, now + 2400);
      }
    }
    const auto state = pacer.snapshot(handle);
    EXPECT_EQ(state->probe.result, paced_probe_result_e::complete);
    EXPECT_EQ(state->probe.successful_groups, 3U);
    EXPECT_EQ(state->probe.successful_packets, 6U);
    EXPECT_EQ(state->probe.successful_ip_bytes, 7200U);
    EXPECT_EQ(state->submitted_ip_bytes, 7200U);
    const auto tail = pacer.dispatch(4801, all_success(4801));
    ASSERT_EQ(tail.successful.size(), 4U);
    for (const auto &success : tail.successful) EXPECT_EQ(success.packet.probe.cluster_id, -1);
    EXPECT_EQ(pacer.snapshot(handle)->submitted_ip_bytes, 12000U);
    EXPECT_EQ(pacer.start_probe(handle, { 9, 8000, 6000, 2000, 3 }, 4801), paced_probe_result_e::invalid);
  }

  TEST(TransportPacer, ProbeRejectsUnavailableMediaDeadlineAndReentryWithoutCreditChanges) {
    deadline_pacer_t pacer;
    const auto handle = *pacer.add_session(7, limits(1000000, 20000), 0);
    EXPECT_EQ(pacer.start_probe(handle, { 1, 8000, 6000, 2000, 3 }, 0), paced_probe_result_e::insufficient_media);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(1, 1, 10, 1200, 9000), 0).result, pacer_enqueue_result_e::queued);
    EXPECT_EQ(pacer.start_probe(handle, { 1, 8000, 6000, 2000, 3 }, 0), paced_probe_result_e::deadline);
    EXPECT_EQ(pacer.start_probe(handle, { 1, 800000, 6000, 2000, 3 }, 0), paced_probe_result_e::budget_deferred);
    auto sent = pacer.dispatch(0, [&](auto, auto packets) {
      EXPECT_EQ(pacer.start_probe(handle, { 1, 8000, 6000, 2000, 3 }, 0), paced_probe_result_e::invalid);
      EXPECT_FALSE(pacer.cancel_probe(handle, 0));
      return all_success(0)(handle, packets);
    });
    ASSERT_EQ(sent.successful.size(), 10U);
    for (const auto &success : sent.successful) EXPECT_EQ(success.packet.probe.cluster_id, -1);
    EXPECT_EQ(pacer.snapshot(handle)->budget_debt_bytes, 0U);
  }

  TEST(TransportPacer, ProbeCancellationAndLateScheduleNeverDelayTheRemainingMedia) {
    for (const bool late : { false, true }) {
      deadline_pacer_t pacer;
      const auto handle = *pacer.add_session(7, limits(1000000, 20000), 0);
      ASSERT_EQ(pacer.enqueue_frame(handle, frame(1, 1, 10), 0).result, pacer_enqueue_result_e::queued);
      ASSERT_EQ(pacer.start_probe(handle, { 1, 8000, 6000, 2000, 3 }, 0), paced_probe_result_e::active);
      ASSERT_EQ(pacer.dispatch(0, all_success(0)).successful.size(), 2U);
      if (!late) {
        ASSERT_TRUE(pacer.cancel_probe(handle, 1));
      }
      const auto now = late ? 7401 : 1;
      const auto sent = pacer.dispatch(now, all_success(now));
      ASSERT_EQ(sent.successful.size(), 8U);
      for (const auto &success : sent.successful) EXPECT_EQ(success.packet.probe.cluster_id, -1);
      EXPECT_EQ(pacer.snapshot(handle)->probe.result, late ? paced_probe_result_e::schedule_late : paced_probe_result_e::cancelled);
      EXPECT_EQ(pacer.snapshot(handle)->probe.successful_packets, 2U);
    }
  }

  TEST(TransportPacer, ProbeDeferralPartialAndUnknownSubmissionKeepOnlyRealPrefixTags) {
    for (const auto mode : { 0, 1, 2 }) {
      deadline_pacer_t pacer;
      const auto handle = *pacer.add_session(7, limits(1000000, 20000), 0);
      ASSERT_EQ(pacer.enqueue_frame(handle, frame(1, 1, 10), 0).result, pacer_enqueue_result_e::queued);
      ASSERT_EQ(pacer.start_probe(handle, { 1, 8000, 6000, 2000, 3 }, 0), paced_probe_result_e::active);
      const auto sent = pacer.dispatch(0, [&](auto, auto packets) {
        EXPECT_EQ(packets.size(), 2U);
        return paced_batch_submission_t { { { mode != 0, 0 }, { false, 0 } }, 0, true, mode != 2, mode == 0 };
      });
      EXPECT_EQ(sent.successful.size(), mode == 0 ? 0U : 1U);
      EXPECT_EQ(pacer.snapshot(handle)->probe.successful_packets, mode == 0 ? 0U : 1U);
      EXPECT_EQ(pacer.snapshot(handle)->probe.result, mode == 0 ? paced_probe_result_e::budget_deferred : paced_probe_result_e::send_failed);
      EXPECT_EQ(sent.accounting_closed, mode == 2);
      if (mode != 2) {
        const auto retry = pacer.dispatch(1000, all_success(1000));
        for (const auto &success : retry.successful) {
          EXPECT_EQ(success.packet.probe.cluster_id, -1);
          EXPECT_GT(success.packet.extended_sequence, mode == 0 ? 0U : 1U);
        }
      }
    }
  }

  TEST(TransportPacer, ProbeWaitDoesNotBlockAnotherFlowOrResetSharedHostCredit) {
    deadline_pacer_t pacer;
    const auto a = *pacer.add_session(7, limits(1000000, 20000), 0);
    const auto b = *pacer.add_session(8, limits(1000000, 20000), 0);
    ASSERT_TRUE(pacer.set_host_limits(limits(1000000, 10000), 0));
    ASSERT_EQ(pacer.enqueue_frame(a, frame(1, 1, 10), 0).result, pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.enqueue_frame(b, frame(1, 20, 2, 1200, 1000000, frame_dependency_e::non_reference, policy(8)), 0).result, pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.start_probe(a, { 1, 8000, 6000, 2000, 3 }, 0), paced_probe_result_e::active);
    ASSERT_EQ(pacer.dispatch(0, all_success(0)).successful.size(), 2U);
    const auto peer = pacer.dispatch(1, all_success(1));
    ASSERT_EQ(peer.successful.size(), 2U);
    EXPECT_EQ(peer.successful.front().session_handle, b);
    EXPECT_EQ(pacer.host_snapshot().submitted_ip_bytes, 4800U);
    EXPECT_EQ(pacer.host_snapshot().budget_debt_bytes, 0U);
  }

  TEST(TransportPacer, ProbeNeverMintsCreditAfterLowerLimitsOrLabelsAnUnderfundedGroup) {
    deadline_pacer_t pacer;
    const auto handle = *pacer.add_session(7, limits(1000000, 20000), 0);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(1, 1, 10), 0).result, pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.start_probe(handle, { 1, 8000, 6000, 2000, 3 }, 0), paced_probe_result_e::active);
    ASSERT_EQ(pacer.dispatch(0, all_success(0)).successful.size(), 2U);
    ASSERT_TRUE(pacer.update_limits(handle, limits(1000000, 1200), 1));
    const auto normal = pacer.dispatch(2400, all_success(2400));
    ASSERT_EQ(normal.successful.size(), 1U);
    EXPECT_EQ(normal.successful[0].packet.probe.cluster_id, -1);
    EXPECT_EQ(pacer.snapshot(handle)->probe.result, paced_probe_result_e::budget_deferred);
    EXPECT_EQ(pacer.snapshot(handle)->probe.successful_ip_bytes, 2400U);
    EXPECT_EQ(pacer.snapshot(handle)->submitted_ip_bytes, 3600U);
    EXPECT_EQ(pacer.snapshot(handle)->budget_debt_bytes, 0U);
  }

  TEST(TransportPacer, LastMomentProbeRevocationRetriesOrdinaryMediaWithoutFalseOsFailures) {
    deadline_pacer_t pacer;
    const auto handle = *pacer.add_session(7, limits(1000000, 20000), 0);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(1, 1, 10), 0).result, pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.start_probe(handle, { 1, 8000, 6000, 2000, 3 }, 0), paced_probe_result_e::active);
    const auto declined = pacer.dispatch(0, [](auto, auto packets) {
      paced_batch_submission_t result;
      result.packets.resize(packets.size());
      result.suffix_probe_cancelled = true;
      return result;
    });
    EXPECT_TRUE(declined.successful.empty());
    EXPECT_FALSE(declined.accounting_closed);
    EXPECT_EQ(pacer.snapshot(handle)->probe.result, paced_probe_result_e::cancelled);
    const auto ordinary = pacer.dispatch(1000, all_success(1000));
    ASSERT_EQ(ordinary.successful.size(), 10U);
    for (const auto &success : ordinary.successful) EXPECT_EQ(success.packet.probe.cluster_id, -1);
    EXPECT_EQ(pacer.snapshot(handle)->submitted_ip_bytes, 12000U);
  }

  TEST(TransportPacer, ProbeWaitsForAnEntireFundableGroupWithinSlackWithoutInventingSuccess) {
    deadline_pacer_t pacer;
    const auto handle = *pacer.add_session(7, limits(1000000, 10000), 0);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(1, 1, 10), 0).result, pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.start_probe(handle, { 1, 8000, 6000, 2000, 3 }, 0), paced_probe_result_e::active);
    ASSERT_EQ(pacer.dispatch(0, all_success(0)).successful.size(), 2U);
    ASSERT_TRUE(pacer.debit_external_success(handle, 8000, 2399));
    const auto waiting = pacer.dispatch(2400, all_success(2400));
    EXPECT_FALSE(waiting.attempted);
    EXPECT_TRUE(waiting.successful.empty());
    ASSERT_TRUE(waiting.next_wakeup_us);
    EXPECT_EQ(*waiting.next_wakeup_us, 2800);
    EXPECT_EQ(pacer.snapshot(handle)->probe.successful_packets, 2U);
    EXPECT_EQ(pacer.snapshot(handle)->probe.result, paced_probe_result_e::active);
    const auto next = pacer.dispatch(2800, all_success(2800));
    ASSERT_EQ(next.successful.size(), 2U);
    EXPECT_EQ(pacer.snapshot(handle)->probe.successful_packets, 4U);
    EXPECT_EQ(pacer.snapshot(handle)->externally_submitted_ip_bytes, 8000U);
    EXPECT_EQ(pacer.snapshot(handle)->submitted_ip_bytes, 4800U);
    EXPECT_EQ(pacer.snapshot(handle)->budget_debt_bytes, 0U);
  }

  TEST(TransportPacer, HighRateProbeRotatesBetweenBoundedBatchesAndCountsOnlyWholeGroups) {
    deadline_pacer_t pacer;
    const auto a = *pacer.add_session(7, limits(12500000, 32768), 0);
    const auto b = *pacer.add_session(8, limits(12500000, 32768), 0);
    ASSERT_TRUE(pacer.set_host_limits(limits(12500000, 32768), 0));
    ASSERT_EQ(pacer.enqueue_frame(a, frame(1, 1, 70), 0).result, pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.enqueue_frame(b, frame(1, 100, 1, 1200, 1000000, frame_dependency_e::non_reference, policy(8)), 0).result, pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.start_probe(a, { 1, 100000, 6000, 2000, 3 }, 0), paced_probe_result_e::active);
    std::set<std::uint64_t> identities;
    auto now = 0LL;
    for (unsigned group = 0; group < 3; ++group) {
      const auto first = pacer.dispatch(now, all_success(now));
      ASSERT_EQ(first.successful.size(), 13U);
      EXPECT_EQ(pacer.snapshot(a)->probe.successful_groups, group);
      for (const auto &sent : first.successful) {
        EXPECT_EQ(sent.session_handle, a);
        EXPECT_EQ(sent.packet.probe.cluster_id, 1);
        EXPECT_TRUE(identities.insert(sent.packet.extended_sequence).second);
      }
      if (group == 0) {
        ++now;
        const auto peer = pacer.dispatch(now, all_success(now));
        ASSERT_EQ(peer.successful.size(), 1U);
        EXPECT_EQ(peer.successful[0].session_handle, b);
        EXPECT_EQ(peer.successful[0].packet.probe.cluster_id, -1);
      }
      ++now;
      const auto second = pacer.dispatch(now, all_success(now));
      ASSERT_EQ(second.successful.size(), 8U);
      for (const auto &sent : second.successful) {
        EXPECT_EQ(sent.session_handle, a);
        EXPECT_TRUE(identities.insert(sent.packet.extended_sequence).second);
      }
      const auto state = pacer.snapshot(a);
      EXPECT_EQ(state->probe.successful_groups, group + 1);
      EXPECT_EQ(state->probe.successful_ip_bytes, (group + 1) * 25200U);
      if (group != 2) {
        EXPECT_EQ(state->probe.next_send_us, now + 2016);
        EXPECT_FALSE(pacer.dispatch(now + 1, all_success(now + 1)).attempted);
        now = state->probe.next_send_us;
      }
    }
    EXPECT_EQ(pacer.snapshot(a)->probe.result, paced_probe_result_e::complete);
    EXPECT_EQ(identities.size(), 63U);
    EXPECT_EQ(pacer.host_snapshot().submitted_ip_bytes, 76800U);
    EXPECT_TRUE(pacer.host_snapshot().accounting_valid);
    EXPECT_EQ(pacer.host_snapshot().budget_debt_bytes, 0U);
    ++now;
    const auto tail = pacer.dispatch(now, all_success(now));
    ASSERT_EQ(tail.successful.size(), 5U);
    for (const auto &sent : tail.successful) EXPECT_EQ(sent.packet.probe.cluster_id, -1);
    now += 200;
    const auto remainder = pacer.dispatch(now, all_success(now));
    ASSERT_EQ(remainder.successful.size(), 2U);
    for (const auto &sent : remainder.successful) EXPECT_EQ(sent.packet.probe.cluster_id, -1);
    EXPECT_EQ(pacer.host_snapshot().submitted_ip_bytes, 85200U);
  }

  TEST(TransportPacer, ProbePacketBatchLimitDoesNotChangeNativeGroupSize) {
    pacer_bounds_t bounds;
    bounds.maximum_batch_packets = 1;
    deadline_pacer_t pacer(bounds);
    const auto handle = *pacer.add_session(7, limits(1000000, 20000), 0);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(1, 1, 6), 0).result, pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.start_probe(handle, { 1, 8000, 6000, 2000, 3 }, 0), paced_probe_result_e::active);
    for (const auto now : { 0, 1, 2401, 2402, 4802, 4803 }) {
      ASSERT_EQ(pacer.dispatch(now, all_success(now)).successful.size(), 1U);
      const auto state = pacer.snapshot(handle);
      EXPECT_EQ(state->probe.successful_groups, state->probe.successful_packets / 2);
    }
    EXPECT_EQ(pacer.snapshot(handle)->probe.result, paced_probe_result_e::complete);
    EXPECT_EQ(pacer.snapshot(handle)->probe.successful_groups, 3U);
    EXPECT_EQ(pacer.snapshot(handle)->probe.successful_ip_bytes, 7200U);
  }

  TEST(TransportPacer, SplitProbeGroupRetainsOriginalSlackAndRechecksReducedBudget) {
    for (const bool late : { false, true }) {
      deadline_pacer_t pacer;
      const auto handle = *pacer.add_session(7, limits(12500000, 32768), 0);
      ASSERT_EQ(pacer.enqueue_frame(handle, frame(1, 1, 70), 0).result, pacer_enqueue_result_e::queued);
      ASSERT_EQ(pacer.start_probe(handle, { 1, 100000, 6000, 2000, 3 }, 0), paced_probe_result_e::active);
      ASSERT_EQ(pacer.dispatch(0, all_success(0)).successful.size(), 13U);
      if (!late) {
        ASSERT_TRUE(pacer.update_limits(handle, limits(12500000, 1200), 1));
      }
      const auto now = late ? 5001 : 2;
      const auto ordinary = pacer.dispatch(now, all_success(now));
      ASSERT_FALSE(ordinary.successful.empty());
      for (const auto &sent : ordinary.successful) EXPECT_EQ(sent.packet.probe.cluster_id, -1);
      EXPECT_EQ(pacer.snapshot(handle)->probe.successful_groups, 0U);
      EXPECT_EQ(pacer.snapshot(handle)->probe.successful_ip_bytes, 15600U);
      EXPECT_EQ(pacer.snapshot(handle)->probe.result, late ? paced_probe_result_e::schedule_late : paced_probe_result_e::budget_deferred);
      EXPECT_TRUE(pacer.snapshot(handle)->accounting_valid);
    }
  }

  TEST(TransportPacer, SplitProbeUnknownSubmissionNeverClaimsACompleteGroup) {
    deadline_pacer_t pacer;
    const auto handle = *pacer.add_session(7, limits(12500000, 32768), 0);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(1, 1, 70), 0).result, pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.start_probe(handle, { 1, 100000, 6000, 2000, 3 }, 0), paced_probe_result_e::active);
    const auto sent = pacer.dispatch(0, [&](auto, auto packets) {
      auto result = all_success(0)(handle, packets);
      result.submission_known = false;
      return result;
    });
    EXPECT_TRUE(sent.accounting_closed);
    EXPECT_EQ(sent.successful.size(), 13U);
    EXPECT_EQ(pacer.snapshot(handle)->probe.successful_groups, 0U);
    EXPECT_EQ(pacer.snapshot(handle)->probe.successful_ip_bytes, 15600U);
    EXPECT_EQ(pacer.snapshot(handle)->probe.result, paced_probe_result_e::send_failed);
    EXPECT_FALSE(pacer.dispatch(1, all_success(1)).attempted);
  }

  TEST(TransportPacer, OwnsCiphertextAndPreservesPolicyUntilActualSubmission) {
    deadline_pacer_t pacer;
    auto handle = pacer.add_session(7, limits(), 0);
    ASSERT_TRUE(handle);
    auto snapshot = policy(7, 18);
    auto encoded = frame(42, 100, 2, 1200, 1000000, frame_dependency_e::non_reference, snapshot);
    encoded.packets[0].metadata.probe = { 4, 5, 6000, 8000 };
    ASSERT_EQ(pacer.enqueue_frame(*handle, std::move(encoded), 0).result, pacer_enqueue_result_e::queued);
    snapshot.reset();
    ASSERT_TRUE(pacer.update_limits(*handle, limits(500000), 0));
    auto result = pacer.dispatch(0, [](std::uint64_t, std::span<const paced_packet_view_t> packets) {
      EXPECT_EQ(packets.size(), 2);
      EXPECT_EQ(packets[0].udp_payload.size(), 1172);
      EXPECT_EQ(packets[0].udp_payload[0], 100);
      EXPECT_EQ(packets[1].udp_payload.back(), 101);
      EXPECT_EQ(packets[0].metadata.policy_revision, 18);
      EXPECT_EQ(packets[0].metadata.probe.cluster_id, 4);
      EXPECT_EQ(packets[0].deadline_us, 1000000);
      return paced_batch_submission_t { { { true, 10 }, { true, 11 } }, 12, true };
    });
    ASSERT_EQ(result.successful.size(), 2);
    EXPECT_EQ(result.successful[0].packet.send_time_us, 10);
    EXPECT_EQ(result.successful[1].packet.send_time_us, 11);
    EXPECT_EQ(result.successful[0].policy->revision, 18);
    ASSERT_EQ(result.frames.size(), 1);
    EXPECT_EQ(result.frames[0].result, frame_send_result_e::complete);
    EXPECT_EQ(result.frames[0].submitted_ip_bytes, 2400);
    EXPECT_EQ(result.frames[0].abandoned_ip_bytes, 0);
    EXPECT_EQ(pacer.snapshot(*handle)->queued_payload_bytes, 0);
    EXPECT_EQ(pacer.snapshot(*handle)->queued_packets, 0);
    EXPECT_EQ(pacer.host_snapshot().submitted_ip_bytes, 2400);
  }

  TEST(TransportPacer, IpQueueCountsHeadersAndOnlyUnsubmittedSuffixesAcrossFlows) {
    deadline_pacer_t pacer;
    const auto a = *pacer.add_session(7, limits(), 0);
    const auto b = *pacer.add_session(8, limits(), 0);
    auto mixed = frame(1, 0, 3);
    mixed.packets[1].ipv6 = true;
    mixed.packets[1].udp_payload.resize(1152);
    ASSERT_EQ(pacer.enqueue_frame(a, std::move(mixed), 0).result, pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.enqueue_frame(b, frame(1, 10, 2, 1200, 1000000, frame_dependency_e::non_reference, policy(8), true), 0).result, pacer_enqueue_result_e::queued);
    EXPECT_EQ(pacer.snapshot(a)->queued_ip_bytes, 3600U);
    EXPECT_EQ(pacer.snapshot(b)->queued_ip_bytes, 2400U);
    EXPECT_EQ(pacer.host_snapshot().queued_ip_bytes, 6000U);
    EXPECT_LT(pacer.host_snapshot().queued_payload_bytes, pacer.host_snapshot().queued_ip_bytes);
    const auto partial = pacer.dispatch(0, [](auto, auto packets) {
      EXPECT_EQ(packets.size(), 2U);
      return paced_batch_submission_t { { { true, 0 }, { false, 0 } }, 0, true };
    });
    ASSERT_EQ(partial.successful.size(), 1U);
    EXPECT_EQ(pacer.snapshot(a)->queued_ip_bytes, 2400U);
    EXPECT_EQ(pacer.host_snapshot().queued_ip_bytes, 4800U);
    ASSERT_EQ(pacer.dispatch(0, all_success(0)).successful.size(), 2U);
    EXPECT_EQ(pacer.snapshot(b)->queued_ip_bytes, 0U);
    EXPECT_EQ(pacer.host_snapshot().queued_ip_bytes, 2400U);
    ASSERT_EQ(pacer.dispatch(1200, all_success(1200)).successful.size(), 2U);
    EXPECT_EQ(pacer.host_snapshot().queued_ip_bytes, 0U);
    EXPECT_EQ(pacer.host_snapshot().submitted_ip_bytes, 6000U);
  }

  TEST(TransportPacer, SharedBudgetDeferralDoesNotConsumeOsFailureAttempts) {
    deadline_pacer_t pacer;
    const auto handle = *pacer.add_session(7, limits(), 0);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(1, 0, 1), 0).result, pacer_enqueue_result_e::queued);
    std::int64_t now = 0;
    for (unsigned i = 0; i < 20; ++i) {
      const auto result = pacer.dispatch(now, [now](auto, auto packets) {
        paced_batch_submission_t deferred;
        deferred.packets.resize(packets.size());
        deferred.completed_at_us = now;
        deferred.suffix_budget_deferred = true;
        return deferred;
      });
      EXPECT_TRUE(result.frames.empty());
      ASSERT_TRUE(result.next_wakeup_us);
      EXPECT_GT(*result.next_wakeup_us, now);
      now = *result.next_wakeup_us;
    }
    const auto completed = pacer.dispatch(now, all_success(now));
    ASSERT_EQ(completed.frames.size(), 1U);
    EXPECT_EQ(completed.frames.front().result, frame_send_result_e::complete);
    EXPECT_EQ(pacer.snapshot(handle)->submitted_packets, 1U);
  }

  TEST(TransportPacer, SharedBudgetPrefixDefersOnlyUnattemptedTailWithoutDuplicates) {
    deadline_pacer_t pacer;
    const auto handle = *pacer.add_session(7, limits(1000000, 6000), 0);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(1, 0, 3), 0).result, pacer_enqueue_result_e::queued);
    const auto prefix = pacer.dispatch(0, [](auto, auto packets) {
      paced_batch_submission_t result;
      result.packets.resize(packets.size());
      result.packets[0] = { true, 0 };
      result.suffix_budget_deferred = true;
      return result;
    });
    ASSERT_EQ(prefix.successful.size(), 1U);
    ASSERT_TRUE(prefix.next_wakeup_us);
    const auto tail = pacer.dispatch(*prefix.next_wakeup_us, all_success(*prefix.next_wakeup_us));
    ASSERT_EQ(tail.successful.size(), 2U);
    EXPECT_EQ(tail.successful.front().packet.extended_sequence, 1U);
    EXPECT_EQ(pacer.snapshot(handle)->submitted_ip_bytes, 3600U);
    EXPECT_EQ(tail.frames.front().result, frame_send_result_e::complete);
  }

  TEST(TransportPacer, IpQueueRemovesAbandonedPacketsWithoutCountingThemAsSent) {
    for (const bool unknown : { false, true }) {
      deadline_pacer_t pacer;
      const auto a = *pacer.add_session(7, limits(), 0);
      ASSERT_EQ(pacer.enqueue_frame(a, frame(1, 0, 2), 0).result, pacer_enqueue_result_e::queued);
      if (unknown) {
        const auto result = pacer.dispatch(0, [](auto, auto) {
          return paced_batch_submission_t { { { true, 0 }, { false, 0 } }, 0, false, false };
        });
        EXPECT_TRUE(result.accounting_closed);
        EXPECT_EQ(result.successful.size(), 1U);
        EXPECT_EQ(pacer.host_snapshot().submitted_ip_bytes, 1200U);
      }
      else {
        ASSERT_TRUE(pacer.abort_noexcept());
        EXPECT_EQ(pacer.host_snapshot().submitted_ip_bytes, 0U);
      }
      EXPECT_EQ(pacer.snapshot(a)->queued_ip_bytes, 0U);
      EXPECT_EQ(pacer.host_snapshot().queued_ip_bytes, 0U);
    }
    deadline_pacer_t pacer;
    const auto a = *pacer.add_session(7, limits(), 0);
    ASSERT_EQ(pacer.enqueue_frame(a, frame(1, 0, 2), 0).result, pacer_enqueue_result_e::queued);
    pacer.stop_session(a, 0);
    EXPECT_EQ(pacer.host_snapshot().queued_ip_bytes, 0U);
    EXPECT_EQ(pacer.host_snapshot().submitted_ip_bytes, 0U);
  }

  TEST(TransportPacer, UnknownNativeSuffixKeepsConfirmedPrefixAndClosesOwnerWithoutRetry) {
    deadline_pacer_t pacer;
    const auto handle = *pacer.add_session(7, limits(1000, 1200, 2400), 0);
    const auto other = *pacer.add_session(8, limits(1000, 1200, 2400), 0);
    ASSERT_TRUE(pacer.set_host_limits(limits(1000, 1200, 2400), 0));
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(1, 100, 3), 0).result, pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.enqueue_frame(other, frame(1, 200, 1, 1200, 1000000, frame_dependency_e::non_reference, policy(8)), 0).result, pacer_enqueue_result_e::queued);
    unsigned calls = 0;
    const auto result = pacer.dispatch(0, [&](auto flow, auto packets) {
      ++calls;
      EXPECT_EQ(flow, handle);
      EXPECT_EQ(packets.size(), 3u);
      return paced_batch_submission_t { { { true, 10 }, { true, 10 }, { false, 0 } }, 10, false, false };
    });
    EXPECT_EQ(calls, 1u);
    ASSERT_EQ(result.successful.size(), 2u);
    EXPECT_EQ(result.successful[0].packet.extended_sequence, 100u);
    EXPECT_EQ(result.successful[1].packet.extended_sequence, 101u);
    EXPECT_TRUE(result.sender_contract_broken);
    EXPECT_TRUE(result.accounting_closed);
    EXPECT_FALSE(result.next_wakeup_us);
    ASSERT_EQ(result.frames.size(), 2u);
    EXPECT_EQ(result.frames[0].result, frame_send_result_e::sender_contract_broken);
    EXPECT_EQ(result.frames[0].submitted_packets, 2u);
    EXPECT_EQ(result.frames[0].submitted_ip_bytes, 2400u);
    EXPECT_EQ(result.frames[0].abandoned_packets, 1u);
    EXPECT_EQ(result.frames[0].abandoned_ip_bytes, 1200u);
    const auto stats = *pacer.snapshot(handle);
    EXPECT_EQ(stats.submitted_ip_bytes, 2400u);
    EXPECT_EQ(stats.budget_debt_bytes, 1200u);
    EXPECT_FALSE(stats.accounting_valid);
    EXPECT_TRUE(stats.stopped);
    EXPECT_EQ(stats.queued_payload_bytes, 0u);
    EXPECT_FALSE(pacer.host_snapshot().accounting_valid);
    EXPECT_EQ(pacer.host_snapshot().submitted_ip_bytes, 2400u);
    EXPECT_FALSE(pacer.dispatch(11, [&](auto, auto) { ++calls; return paced_batch_submission_t {}; }).attempted);
    EXPECT_EQ(calls, 1u);
  }

  TEST(TransportPacer, FailedBatchPrefixRetriesOnlyTheUnsubmittedSuffix) {
    deadline_pacer_t pacer;
    const auto handle = *pacer.add_session(7, limits(1000000, 6000), 0);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(1, 10, 3), 0).result, pacer_enqueue_result_e::queued);
    auto first = pacer.dispatch(0, [](auto, auto packets) {
      EXPECT_EQ(packets.size(), 3);
      return paced_batch_submission_t { { { true, 0 }, { false, 0 }, { false, 0 } }, 0, true };
    });
    ASSERT_EQ(first.successful.size(), 1);
    EXPECT_EQ(first.successful[0].packet.extended_sequence, 10);
    EXPECT_EQ(pacer.snapshot(handle)->submitted_ip_bytes, 1200);
    EXPECT_FALSE(pacer.dispatch(999, all_success(999)).attempted);
    auto second = pacer.dispatch(1000, [](auto, auto packets) {
      EXPECT_EQ(packets.size(), 2);
      EXPECT_EQ(packets.front().metadata.extended_sequence, 11);
      return paced_batch_submission_t { { { true, 1000 }, { true, 1001 } }, 1001, true };
    });
    ASSERT_EQ(second.successful.size(), 2);
    ASSERT_EQ(second.frames.size(), 1);
    EXPECT_EQ(second.frames[0].submitted_packets, 3);
    EXPECT_EQ(second.frames[0].result, frame_send_result_e::complete);
    send_ledger_t ledger(7);
    ASSERT_TRUE(ledger.commit_success(first.successful[0].packet));
    for (const auto &sent : second.successful) ASSERT_TRUE(ledger.commit_success(sent.packet));
    EXPECT_EQ(ledger.snapshot().committed_packets, 3);
    EXPECT_EQ(ledger.snapshot().committed_ip_bytes, 3600);
  }

  TEST(TransportPacer, FallbackSuccessHolesAbortTheFrameAndNeverResubmitSuccessfulIdentities) {
    deadline_pacer_t pacer;
    const auto handle = *pacer.add_session(7, limits(1000000, 6000), 0);
    auto reference = frame(1, 1, 4, 1200, 1000000, frame_dependency_e::reference);
    // This case loses primary data, rather than only optional parity holes.
    for (auto &packet : reference.packets) packet.metadata.kind = packet_kind_e::data;
    ASSERT_EQ(pacer.enqueue_frame(handle, std::move(reference), 0).result, pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(2, 5, 1, 1200, 1000000, frame_dependency_e::reference), 0).result, pacer_enqueue_result_e::queued);
    auto result = pacer.dispatch(0, [](auto, auto) {
      return paced_batch_submission_t { { { true, 1 }, { false, 0 }, { true, 3 }, { false, 0 } }, 4, true };
    });
    ASSERT_EQ(result.successful.size(), 2);
    EXPECT_EQ(result.successful[0].packet.extended_sequence, 1);
    EXPECT_EQ(result.successful[1].packet.extended_sequence, 3);
    ASSERT_EQ(result.frames.size(), 1);
    EXPECT_EQ(result.frames[0].result, frame_send_result_e::send_failed);
    EXPECT_EQ(result.frames[0].submitted_packets, 2);
    EXPECT_EQ(result.frames[0].abandoned_packets, 2);
    EXPECT_EQ(result.frames[0].submitted_ip_bytes, 2400);
    EXPECT_EQ(result.frames[0].abandoned_ip_bytes, 2400);
    EXPECT_TRUE(result.frames[0].recovery_required);
    auto next = pacer.dispatch(4, [](auto, auto) {
      ADD_FAILURE() << "broken dependent chain must not reach the socket";
      return paced_batch_submission_t {};
    });
    ASSERT_EQ(next.frames.size(), 1);
    EXPECT_EQ(next.frames[0].result, frame_send_result_e::reference_chain_broken);
    EXPECT_EQ(pacer.snapshot(handle)->submitted_ip_bytes, 2400);
    EXPECT_EQ(pacer.snapshot(handle)->queued_payload_bytes, 0);
    EXPECT_EQ(pacer.host_snapshot().queued_packets, 0);
    send_ledger_t ledger(7);
    for (const auto &sent : result.successful) ASSERT_TRUE(ledger.commit_success(sent.packet));
    EXPECT_EQ(ledger.snapshot().committed_packets, 2);
  }

  TEST(TransportPacer, FailureRetryCountAndBackoffAreBoundedAndDoNotSpendBudget) {
    deadline_pacer_t pacer;
    const auto handle = *pacer.add_session(7, limits(0, 1200), 0);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(1, 1, 1), 0).result, pacer_enqueue_result_e::queued);
    for (std::int64_t time = 0; time <= 2000; time += 1000) {
      auto result = pacer.dispatch(time, [time](auto, auto) {
        return paced_batch_submission_t { { { false, 0 } }, time, true };
      });
      ASSERT_TRUE(result.attempted);
      if (time < 2000)
        EXPECT_TRUE(result.frames.empty());
      else {
        ASSERT_EQ(result.frames.size(), 1);
        EXPECT_EQ(result.frames[0].result, frame_send_result_e::send_failed);
        EXPECT_EQ(result.frames[0].submitted_packets, 0);
      }
    }
    EXPECT_EQ(pacer.snapshot(handle)->submitted_ip_bytes, 0);
    EXPECT_EQ(pacer.snapshot(handle)->budget_debt_bytes, 0);
    EXPECT_TRUE(pacer.external_allowance(handle, 1200, 2000));
  }

  TEST(TransportPacer, BudgetReductionRetainsDebtAndDoesNotRewriteTheOldFramePolicy) {
    deadline_pacer_t pacer;
    const auto handle = *pacer.add_session(7, limits(0, 1000, 1000), 0);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(1, 1, 1, 1500), 0).result, pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.dispatch(0, all_success(0)).successful.size(), 1);
    EXPECT_EQ(pacer.snapshot(handle)->budget_debt_bytes, 500);
    ASSERT_TRUE(pacer.update_limits(handle, limits(1000, 1000, 0), 10));
    EXPECT_EQ(pacer.snapshot(handle)->budget_debt_bytes, 500);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(2, 2, 1, 1000, 2000010, frame_dependency_e::non_reference, policy(7, 1)), 10).result, pacer_enqueue_result_e::queued);
    auto waiting = pacer.dispatch(10, all_success(10));
    EXPECT_FALSE(waiting.attempted);
    ASSERT_TRUE(waiting.next_wakeup_us);
    EXPECT_EQ(*waiting.next_wakeup_us, 1500010);
    EXPECT_FALSE(pacer.dispatch(1500009, all_success(1500009)).attempted);
    auto sent = pacer.dispatch(1500010, all_success(1500010));
    ASSERT_EQ(sent.successful.size(), 1);
    EXPECT_EQ(sent.successful[0].policy->revision, 1);
    EXPECT_EQ(pacer.snapshot(handle)->budget_debt_bytes, 0);
    EXPECT_EQ(pacer.snapshot(handle)->submitted_ip_bytes, 2500);
  }

  TEST(TransportPacer, RateChangeIntegratesOldAndNewRatesAtTheirOwnTimeBoundaries) {
    deadline_pacer_t pacer;
    const auto handle = *pacer.add_session(7, limits(1000, 1000), 0);
    ASSERT_TRUE(pacer.debit_external_success(handle, 1000, 0));
    ASSERT_TRUE(pacer.update_limits(handle, limits(2000, 1000), 250000));
    EXPECT_TRUE(pacer.external_allowance(handle, 250, 250000));
    EXPECT_FALSE(pacer.external_allowance(handle, 251, 250000));
    EXPECT_TRUE(pacer.external_allowance(handle, 750, 500000));
    EXPECT_FALSE(pacer.external_allowance(handle, 751, 500000));
    ASSERT_TRUE(pacer.update_limits(handle, limits(0, 100), 500000));
    EXPECT_TRUE(pacer.external_allowance(handle, 100, 500000));
    EXPECT_FALSE(pacer.external_allowance(handle, 101, 500000));
  }

  TEST(TransportPacer, FeasibilityAbortsWholeOldFrameBeforeSendingWhenTheBudgetFalls) {
    pacer_bounds_t bounds;
    bounds.maximum_batch_packets = 1;
    deadline_pacer_t pacer(bounds);
    const auto handle = *pacer.add_session(7, limits(1000000, 1200), 0);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(1, 1, 3, 1200, 10000, frame_dependency_e::reference), 0).result, pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(2, 4, 1, 1200, 20000, frame_dependency_e::reference), 0).result, pacer_enqueue_result_e::queued);
    auto first = pacer.dispatch(0, all_success(0));
    ASSERT_EQ(first.successful.size(), 1);
    ASSERT_TRUE(pacer.update_limits(handle, limits(1000, 1200), 1));
    auto aborted = pacer.dispatch(1, [](auto, auto) {
      ADD_FAILURE() << "remaining old frame cannot finish before its deadline";
      return paced_batch_submission_t {};
    });
    ASSERT_EQ(aborted.frames.size(), 2);
    EXPECT_EQ(aborted.frames[0].result, frame_send_result_e::cannot_meet_deadline);
    EXPECT_EQ(aborted.frames[0].submitted_packets, 1);
    EXPECT_EQ(aborted.frames[0].abandoned_packets, 2);
    EXPECT_EQ(aborted.frames[1].result, frame_send_result_e::reference_chain_broken);
    EXPECT_EQ(pacer.snapshot(handle)->submitted_ip_bytes, 1200);
    EXPECT_TRUE(pacer.snapshot(handle)->reference_chain_broken);
  }

  TEST(TransportPacer, ExpiredReferenceAndDependentFramesWaitForACompleteRecovery) {
    deadline_pacer_t pacer;
    const auto handle = *pacer.add_session(7, limits(1000000, 10000), 0);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(1, 1, 1, 1200, 5, frame_dependency_e::reference), 0).result, pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(2, 2, 1, 1200, 1000, frame_dependency_e::reference), 0).result, pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(3, 3, 1, 1200, 1000, frame_dependency_e::non_reference), 0).result, pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(4, 4, 2, 1200, 1000, frame_dependency_e::recovery), 0).result, pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(5, 6, 1, 1200, 1000, frame_dependency_e::reference), 0).result, pacer_enqueue_result_e::queued);
    auto first = pacer.dispatch(5, all_success(5));
    ASSERT_EQ(first.frames.size(), 3);
    EXPECT_EQ(first.frames[0].result, frame_send_result_e::deadline_expired);
    EXPECT_EQ(first.frames[1].result, frame_send_result_e::reference_chain_broken);
    EXPECT_EQ(first.frames[2].result, frame_send_result_e::complete);
    EXPECT_TRUE(pacer.snapshot(handle)->reference_chain_broken);
    auto recovery = pacer.dispatch(5, all_success(5));
    ASSERT_EQ(recovery.frames.size(), 1);
    EXPECT_EQ(recovery.frames[0].result, frame_send_result_e::complete);
    EXPECT_FALSE(recovery.frames[0].recovery_required);
    EXPECT_FALSE(pacer.snapshot(handle)->reference_chain_broken);
    auto restored = pacer.dispatch(5, all_success(5));
    ASSERT_EQ(restored.successful.size(), 1);
    EXPECT_EQ(restored.successful[0].packet.frame_id, 5);
  }

  TEST(TransportPacer, RejectedFutureReferenceDoesNotInvalidateEarlierQueuedFramesOrAnOlderRecovery) {
    pacer_bounds_t bounds;
    bounds.maximum_frames_per_session = 1;
    deadline_pacer_t pacer(bounds);
    const auto handle = *pacer.add_session(7, limits(1000000, 10000), 0);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(1, 1, 1, 1200, 1000, frame_dependency_e::recovery), 0).result, pacer_enqueue_result_e::queued);
    auto dropped = pacer.enqueue_frame(handle, frame(3, 3, 1, 1200, 1000, frame_dependency_e::reference), 0);
    ASSERT_EQ(dropped.result, pacer_enqueue_result_e::queue_full);
    ASSERT_TRUE(dropped.dropped_frame);
    EXPECT_TRUE(dropped.dropped_frame->recovery_required);
    auto old = pacer.dispatch(0, all_success(0));
    ASSERT_EQ(old.successful.size(), 1);
    EXPECT_TRUE(pacer.snapshot(handle)->reference_chain_broken);
    EXPECT_EQ(pacer.enqueue_frame(handle, frame(3, 3, 1), 0).result, pacer_enqueue_result_e::invalid);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(4, 4, 1, 1200, 1000, frame_dependency_e::reference), 0).result, pacer_enqueue_result_e::queued);
    auto dependent = pacer.dispatch(0, all_success(0));
    EXPECT_FALSE(dependent.attempted);
    EXPECT_EQ(dependent.frames[0].result, frame_send_result_e::reference_chain_broken);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(5, 5, 1, 1200, 1000, frame_dependency_e::recovery), 0).result, pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.dispatch(0, all_success(0)).successful.size(), 1);
    EXPECT_FALSE(pacer.snapshot(handle)->reference_chain_broken);
  }

  TEST(TransportPacer, DeadlineViolationByTheSocketIsAccountedAndNeverDeclaredComplete) {
    deadline_pacer_t pacer;
    const auto handle = *pacer.add_session(7, limits(), 0);
    auto reference = frame(1, 1, 2, 1200, 1000, frame_dependency_e::reference);
    // The deadline violation is a primary packet, not optional FEC.
    for (auto &packet : reference.packets) packet.metadata.kind = packet_kind_e::data;
    ASSERT_EQ(pacer.enqueue_frame(handle, std::move(reference), 0).result, pacer_enqueue_result_e::queued);
    auto result = pacer.dispatch(0, [](auto, auto) {
      return paced_batch_submission_t { { { true, 999 }, { true, 1000 } }, 1000, true };
    });
    ASSERT_EQ(result.successful.size(), 2);
    ASSERT_EQ(result.frames.size(), 1);
    EXPECT_EQ(result.frames[0].result, frame_send_result_e::deadline_expired);
    EXPECT_EQ(result.frames[0].submitted_ip_bytes, 2400);
    EXPECT_TRUE(result.frames[0].recovery_required);
    EXPECT_TRUE(pacer.dispatch(999, all_success(999)).clock_invalid);
  }

  TEST(TransportPacer, SessionRoundRobinBoundsBurstsAndOneSocketFailureDoesNotBlockPeers) {
    pacer_bounds_t bounds;
    bounds.batch_quantum_ip_bytes = 2400;
    deadline_pacer_t pacer(bounds);
    const auto a = *pacer.add_session(7, limits(1000000, 10000), 0);
    const auto b = *pacer.add_session(8, limits(1000000, 10000), 0);
    ASSERT_EQ(pacer.enqueue_frame(a, frame(1, 1, 6), 0).result, pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.enqueue_frame(b, frame(1, 1, 6, 1200, 1000000, frame_dependency_e::non_reference, policy(8)), 0).result, pacer_enqueue_result_e::queued);
    auto failed = pacer.dispatch(0, [a](auto session, auto packets) {
      EXPECT_EQ(session, a);
      EXPECT_EQ(packets.size(), 2);
      return paced_batch_submission_t { { { false, 0 }, { false, 0 } }, 0, true };
    });
    EXPECT_TRUE(failed.attempted);
    std::vector<std::uint64_t> turns;
    for (int i = 0; i < 3; ++i) {
      auto sent = pacer.dispatch(0, [&turns](auto session, auto packets) {
        turns.push_back(session);
        EXPECT_LE(packets.size(), 2);
        paced_batch_submission_t value;
        value.completed_at_us = 0;
        value.packets.assign(packets.size(), { true, 0 });
        return value;
      });
      ASSERT_TRUE(sent.attempted);
    }
    ASSERT_EQ(turns.size(), 3);
    EXPECT_TRUE(std::all_of(turns.begin(), turns.end(), [b](auto session) { return session == b; }));
    EXPECT_EQ(pacer.snapshot(b)->submitted_packets, 6);
    EXPECT_EQ(pacer.snapshot(a)->submitted_packets, 0);
    auto retried = pacer.dispatch(1000, all_success(1000));
    ASSERT_EQ(retried.successful.size(), 2);
    EXPECT_EQ(retried.successful[0].session_handle, a);
  }

  TEST(TransportPacer, ReadyFlowsTakeAlternatingBoundedByteQuanta) {
    pacer_bounds_t bounds;
    bounds.batch_quantum_ip_bytes = 2400;
    deadline_pacer_t pacer(bounds);
    const auto a = *pacer.add_session(7, limits(1000000, 10000), 0);
    const auto b = *pacer.add_session(8, limits(1000000, 10000), 0);
    ASSERT_EQ(pacer.enqueue_frame(a, frame(1, 1, 6), 0).result, pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.enqueue_frame(b, frame(1, 1, 6, 1200, 1000000, frame_dependency_e::non_reference, policy(8)), 0).result, pacer_enqueue_result_e::queued);
    for (int i = 0; i < 6; ++i) {
      auto result = pacer.dispatch(0, all_success(0));
      ASSERT_EQ(result.successful.size(), 2);
      EXPECT_EQ(result.successful[0].session_handle, i % 2 == 0 ? a : b);
    }
    EXPECT_EQ(pacer.host_snapshot().submitted_ip_bytes, 14400);
  }

  TEST(TransportPacer, HostBudgetCannotBeStolenByAnEndlessSmallerPacketFlow) {
    deadline_pacer_t pacer;
    ASSERT_TRUE(pacer.set_host_limits(limits(1000, 1500), 0));
    const auto a = *pacer.add_session(7, limits(1000000, 10000), 0);
    const auto b = *pacer.add_session(8, limits(1000000, 10000), 0);
    ASSERT_TRUE(pacer.external_allowance(a, 1500, 0));
    ASSERT_TRUE(pacer.debit_external_success(a, 1500, 0));
    ASSERT_EQ(pacer.enqueue_frame(a, frame(1, 1, 1, 1500, 10000000), 0).result, pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.enqueue_frame(b, frame(1, 1, 20, 100, 10000000, frame_dependency_e::non_reference, policy(8)), 0).result, pacer_enqueue_result_e::queued);
    for (std::int64_t now = 100000; now < 1500000; now += 100000) {
      auto waiting = pacer.dispatch(now, all_success(now));
      EXPECT_FALSE(waiting.attempted);
      ASSERT_TRUE(waiting.next_wakeup_us);
      EXPECT_EQ(*waiting.next_wakeup_us, 1500000);
    }
    auto large = pacer.dispatch(1500000, all_success(1500000));
    ASSERT_EQ(large.successful.size(), 1);
    EXPECT_EQ(large.successful[0].session_handle, a);
    auto small = pacer.dispatch(1600000, all_success(1600000));
    ASSERT_EQ(small.successful.size(), 1);
    EXPECT_EQ(small.successful[0].session_handle, b);
    EXPECT_EQ(pacer.host_snapshot().submitted_ip_bytes + pacer.host_snapshot().externally_submitted_ip_bytes, 3100);
  }

  TEST(TransportPacer, ExternalAudioBytesShareTheSameSuccessfulIpBudget) {
    deadline_pacer_t pacer;
    const auto handle = *pacer.add_session(7, limits(100000, 2400), 0);
    ASSERT_TRUE(pacer.external_allowance(handle, 600, 0));
    ASSERT_TRUE(pacer.debit_external_success(handle, 600, 0));
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(1, 1, 2, 1200, 20000), 0).result, pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.dispatch(0, all_success(0)).successful.size(), 1);
    auto waiting = pacer.dispatch(0, all_success(0));
    EXPECT_FALSE(waiting.attempted);
    ASSERT_TRUE(waiting.next_wakeup_us);
    EXPECT_EQ(*waiting.next_wakeup_us, 6000);
    ASSERT_EQ(pacer.dispatch(6000, all_success(6000)).successful.size(), 1);
    auto snapshot = pacer.snapshot(handle);
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot->externally_submitted_ip_bytes, 600);
    EXPECT_EQ(snapshot->submitted_ip_bytes, 2400);
    EXPECT_TRUE(snapshot->accounting_valid);
    EXPECT_EQ(snapshot->budget_debt_bytes, 0);
  }

  TEST(TransportPacer, UnapprovedExternalSendRecordsActualBytesAndClosesAccounting) {
    deadline_pacer_t pacer;
    const auto handle = *pacer.add_session(7, limits(0, 1200, 100), 0);
    EXPECT_FALSE(pacer.external_allowance(handle, 2000, 0));
    EXPECT_FALSE(pacer.debit_external_success(handle, 2000, 0));
    const auto snapshot = pacer.snapshot(handle);
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot->externally_submitted_ip_bytes, 2000);
    EXPECT_EQ(snapshot->budget_debt_bytes, 800);
    EXPECT_FALSE(snapshot->accounting_valid);
    EXPECT_TRUE(snapshot->stopped);
    EXPECT_TRUE(pacer.host_snapshot().stopped);
    EXPECT_FALSE(pacer.add_session(8, limits(), 0));
    EXPECT_FALSE(pacer.external_allowance(handle, 1, 1000000));
  }

  TEST(TransportPacer, PayloadPacketFrameAndSessionBoundsAreIndependentAndStopReleasesOwnership) {
    pacer_bounds_t bounds;
    bounds.maximum_sessions = 1;
    bounds.maximum_queued_payload_bytes = 1000;
    bounds.maximum_queued_packets = 2;
    deadline_pacer_t pacer(bounds);
    const auto handle = *pacer.add_session(7, limits(), 0);
    EXPECT_FALSE(pacer.add_session(8, limits(), 0));
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(1, 1, 2, 100), 0).result, pacer_enqueue_result_e::queued);
    EXPECT_EQ(pacer.enqueue_frame(handle, frame(2, 3, 1, 100), 0).result, pacer_enqueue_result_e::queue_full);
    EXPECT_EQ(pacer.snapshot(handle)->queued_packets, 2);
    auto stopped = pacer.stop_session(handle, 0);
    ASSERT_EQ(stopped.size(), 1);
    EXPECT_EQ(stopped[0].result, frame_send_result_e::stopped);
    EXPECT_EQ(stopped[0].abandoned_packets, 2);
    EXPECT_EQ(pacer.host_snapshot().queued_payload_bytes, 0);
    EXPECT_EQ(pacer.host_snapshot().queued_packets, 0);
    EXPECT_FALSE(pacer.snapshot(handle));
    const auto fresh = pacer.add_session(8, limits(), 0);
    ASSERT_TRUE(fresh);
    EXPECT_GT(*fresh, handle);
    EXPECT_EQ(pacer.enqueue_frame(*fresh, frame(1, 1, 1, 1100, 1000000, frame_dependency_e::non_reference, policy(8)), 0).result, pacer_enqueue_result_e::queue_full);
    pacer.stop(0);
    EXPECT_FALSE(pacer.add_session(8, limits(), 0));
    EXPECT_EQ(pacer.enqueue_frame(*fresh, frame(2, 2, 1), 0).result, pacer_enqueue_result_e::stopped);
  }

  TEST(TransportPacer, CompleteUdpIpAccountingIncludesIpv6AndRejectsMalformedIdentities) {
    deadline_pacer_t pacer;
    const auto handle = *pacer.add_session(7, limits(), 0);
    auto bad = frame(1, 1, 1, 1200, 1000000, frame_dependency_e::non_reference, policy(), true);
    --bad.packets[0].metadata.ip_bytes;
    EXPECT_EQ(pacer.enqueue_frame(handle, std::move(bad), 0).result, pacer_enqueue_result_e::invalid);
    auto mixed = frame(1, 1, 2);
    mixed.packets[1].metadata.extended_sequence = 1;
    EXPECT_EQ(pacer.enqueue_frame(handle, std::move(mixed), 0).result, pacer_enqueue_result_e::invalid);
    auto wrong = frame(1, 1, 1);
    wrong.policy = policy(8);
    EXPECT_EQ(pacer.enqueue_frame(handle, std::move(wrong), 0).result, pacer_enqueue_result_e::invalid);
    auto ipv6 = frame(1, (1ULL << 24) * 5 + 65535, 2, 1200, 1000000, frame_dependency_e::non_reference, policy(), true);
    ASSERT_EQ(pacer.enqueue_frame(handle, std::move(ipv6), 0).result, pacer_enqueue_result_e::queued);
    auto result = pacer.dispatch(0, all_success(0));
    ASSERT_EQ(result.successful.size(), 2);
    EXPECT_EQ(result.successful[0].packet.ip_bytes, 1200);
    EXPECT_EQ(result.successful[1].packet.extended_sequence, (1ULL << 24) * 5 + 65536);
    EXPECT_EQ(pacer.enqueue_frame(handle, frame(2, 2, 1), 0).result, pacer_enqueue_result_e::invalid);
  }

  TEST(TransportPacer, InvalidSenderContractCannotClaimAReconstructedByteLedger) {
    deadline_pacer_t pacer;
    const auto handle = *pacer.add_session(7, limits(), 0);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(1, 1, 2), 0).result, pacer_enqueue_result_e::queued);
    auto result = pacer.dispatch(0, [](auto, auto) { return paced_batch_submission_t { { { true, 0 } }, 0, true }; });
    EXPECT_TRUE(result.sender_contract_broken);
    EXPECT_TRUE(result.successful.empty());
    ASSERT_EQ(result.frames.size(), 1);
    EXPECT_EQ(result.frames[0].result, frame_send_result_e::sender_contract_broken);
    EXPECT_FALSE(pacer.snapshot(handle)->accounting_valid);
    EXPECT_FALSE(pacer.host_snapshot().accounting_valid);
    EXPECT_FALSE(pacer.dispatch(100, all_success(100)).attempted);
  }

  TEST(TransportPacer, MonotonicClockAndReentrantMutationAreRejected) {
    deadline_pacer_t pacer;
    const auto handle = *pacer.add_session(7, limits(), 100);
    EXPECT_FALSE(pacer.update_limits(handle, limits(), 99));
    EXPECT_EQ(pacer.enqueue_frame(handle, frame(1, 1, 1), 99).result, pacer_enqueue_result_e::clock_invalid);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(1, 1, 1), 100).result, pacer_enqueue_result_e::queued);
    auto result = pacer.dispatch(100, [&pacer, handle](auto, auto) {
      EXPECT_FALSE(pacer.update_limits(handle, limits(), 100));
      EXPECT_TRUE(pacer.stop(100).empty());
      EXPECT_TRUE(pacer.dispatch(100, all_success(100)).clock_invalid);
      return paced_batch_submission_t { { { true, 100 } }, 100, true };
    });
    ASSERT_EQ(result.successful.size(), 1);
    EXPECT_FALSE(pacer.snapshot(handle)->stopped);
  }

  TEST(TransportPacer, LongIdleAndMaximumConfiguredRatesSaturateWithoutOverflowOrUnboundedBurst) {
    deadline_pacer_t pacer;
    const auto handle = *pacer.add_session(7, limits(1000000000000ULL, 1200), 0);
    ASSERT_TRUE(pacer.debit_external_success(handle, 1200, 0));
    const auto late = std::numeric_limits<std::int64_t>::max() - 1;
    EXPECT_TRUE(pacer.external_allowance(handle, 1200, late));
    EXPECT_FALSE(pacer.external_allowance(handle, 1201, late));
    ASSERT_TRUE(pacer.debit_external_success(handle, 1200, late));
    EXPECT_EQ(pacer.snapshot(handle)->budget_debt_bytes, 0);
    EXPECT_EQ(pacer.snapshot(handle)->externally_submitted_ip_bytes, 2400);
    auto bad_limits = limits(1000000000001ULL);
    EXPECT_FALSE(pacer.update_limits(handle, bad_limits, late));
    EXPECT_THROW(deadline_pacer_t(pacer_bounds_t { 0 }), std::invalid_argument);
  }

  TEST(TransportPacer, OversizedPacketCannotBorrowUnboundedDebtToAvoidAFrameDeadline) {
    deadline_pacer_t pacer;
    const auto handle = *pacer.add_session(7, limits(1000000000, 500, 500), 0);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(1, 1, 1, 1200, 1000000, frame_dependency_e::reference), 0).result, pacer_enqueue_result_e::queued);
    auto result = pacer.dispatch(0, [](auto, auto) {
      ADD_FAILURE() << "packet is larger than every allowed instantaneous credit";
      return paced_batch_submission_t {};
    });
    ASSERT_EQ(result.frames.size(), 1);
    EXPECT_EQ(result.frames[0].result, frame_send_result_e::cannot_meet_deadline);
    EXPECT_EQ(pacer.snapshot(handle)->budget_debt_bytes, 0);
  }

  TEST(TransportPacer, IndependentIntervalOracleVerifiesActualBytesAcrossRateStepsFailuresAndProbeKinds) {
    pacer_bounds_t bounds;
    bounds.maximum_batch_packets = 3;
    bounds.batch_quantum_ip_bytes = 2400;
    bounds.maximum_failed_attempts = 2;
    deadline_pacer_t pacer(bounds);
    const auto handle = *pacer.add_session(7, limits(120000, 2400, 1200), 0);
    std::vector<std::pair<std::int64_t, std::uint64_t>> actual;
    std::vector<std::pair<std::int64_t, std::uint64_t>> changes { { 0, 120000 }, { 250000, 25000 }, { 650000, 75000 } };
    std::uint64_t next_frame = 1, next_sequence = 1;
    unsigned attempt = 0;
    for (std::int64_t now = 0; now <= 1000000; now += 500) {
      if (now == 250000) { ASSERT_TRUE(pacer.update_limits(handle, limits(25000, 2400, 1200), now)); }
      if (now == 650000) { ASSERT_TRUE(pacer.update_limits(handle, limits(75000, 2400, 1200), now)); }
      if (pacer.snapshot(handle)->queued_frames < 4) {
        auto next = frame(next_frame++, next_sequence, 3, 1200, now + 250000);
        next_sequence += 3;
        next.packets.back().metadata.kind = packet_kind_e::probe;
        next.packets.back().metadata.probe = { 1, 3, 3600, 960 };
        ASSERT_EQ(pacer.enqueue_frame(handle, std::move(next), now).result, pacer_enqueue_result_e::queued);
      }
      auto result = pacer.dispatch(now, [&actual, &attempt, now](auto, auto packets) {
        paced_batch_submission_t submission;
        submission.completed_at_us = now;
        ++attempt;
        for (std::size_t i = 0; i < packets.size(); ++i) {
          const bool success = attempt % 17 != 0 || i == 0;
          submission.packets.push_back({ success, now });
          if (success) actual.emplace_back(now, packets[i].metadata.ip_bytes);
        }
        return submission;
      });
      ASSERT_FALSE(result.sender_contract_broken);
      ASSERT_LE(pacer.snapshot(handle)->budget_debt_bytes, 1200);
      ASSERT_LE(pacer.snapshot(handle)->instant_debt_bytes, 1200);
    }
    ASSERT_GT(actual.size(), 40);
    std::uint64_t total = 0;
    for (const auto &[time, bytes] : actual) {
      (void) time;
      total += bytes;
    }
    EXPECT_EQ(pacer.snapshot(handle)->submitted_ip_bytes, total);
    // This oracle integrates configured piecewise rates from the experiment
    // schedule, not the pacer's token state or accounting implementation.
    for (std::int64_t begin = 0; begin <= 1000000; begin += 10000) {
      for (std::int64_t end = begin; end <= 1000000; end += 10000) {
        std::uint64_t sent = 0;
        for (const auto &[time, bytes] : actual)
          if (time >= begin && time <= end) sent += bytes;
        std::uint64_t integral_microbytes = 0;
        for (std::size_t i = 0; i < changes.size(); ++i) {
          const auto left = std::max(begin, changes[i].first);
          const auto right = std::min(end, i + 1 < changes.size() ? changes[i + 1].first : end);
          if (right > left) integral_microbytes += static_cast<std::uint64_t>(right - left) * changes[i].second;
        }
        EXPECT_LE(sent * 1000000ULL, integral_microbytes + 3600ULL * 1000000ULL)
          << "interval [" << begin << "," << end << "]";
      }
    }
  }

  TEST(TransportPacer, InstantPacingRateIndependentlyConstrainsTheHigherAverageBudget) {
    deadline_pacer_t pacer;
    auto configured = limits(10000000, 12000);
    configured.instant = { 100000, 1200, 0 };
    const auto handle = *pacer.add_session(7, configured, 0);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(1, 1, 5, 1200, 100000), 0).result, pacer_enqueue_result_e::queued);
    std::vector<sent_packet_t> actual;
    for (std::int64_t now = 0; now <= 48000; now += 100) pacer.dispatch(now, all_success(now, &actual));
    ASSERT_EQ(actual.size(), 5);
    for (std::size_t i = 0; i < actual.size(); ++i) EXPECT_EQ(actual[i].send_time_us, static_cast<std::int64_t>(i) * 12000);
    EXPECT_EQ(pacer.snapshot(handle)->submitted_ip_bytes, 6000);
    EXPECT_EQ(pacer.snapshot(handle)->instant_debt_bytes, 0);
  }

  TEST(TransportPacer, ExpiredQueuedTailIsRetiredWithoutInvalidatingItsEarlierStillValidFrame) {
    deadline_pacer_t pacer;
    const auto handle = *pacer.add_session(7, limits(1000, 1200), 0);
    ASSERT_TRUE(pacer.debit_external_success(handle, 1200, 0));
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(1, 1, 1, 1200, 2000000, frame_dependency_e::reference), 0).result, pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(2, 2, 1, 1200, 1000000, frame_dependency_e::reference), 0).result, pacer_enqueue_result_e::queued);
    auto waiting = pacer.dispatch(0, all_success(0));
    ASSERT_TRUE(waiting.next_wakeup_us);
    EXPECT_EQ(*waiting.next_wakeup_us, 1000000);
    auto expired = pacer.dispatch(1000000, all_success(1000000));
    ASSERT_EQ(expired.frames.size(), 1);
    EXPECT_EQ(expired.frames[0].frame_id, 2);
    EXPECT_EQ(expired.frames[0].result, frame_send_result_e::deadline_expired);
    EXPECT_FALSE(expired.attempted);
    auto earlier = pacer.dispatch(1200000, all_success(1200000));
    ASSERT_EQ(earlier.successful.size(), 1);
    EXPECT_EQ(earlier.successful[0].packet.frame_id, 1);
    EXPECT_TRUE(pacer.snapshot(handle)->reference_chain_broken);
    EXPECT_EQ(pacer.snapshot(handle)->queued_packets, 0);
  }

  TEST(TransportPacer, AccountingAfterTheLastTimelySuccessfulPacketDoesNotFabricateAnExpiredFrame) {
    deadline_pacer_t pacer;
    const auto handle = *pacer.add_session(7, limits(), 0);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(1, 1, 2, 1200, 1000, frame_dependency_e::recovery), 0).result, pacer_enqueue_result_e::queued);
    auto result = pacer.dispatch(0, [](auto, auto) {
      return paced_batch_submission_t { { { true, 998 }, { true, 999 } }, 1001, true };
    });
    ASSERT_EQ(result.frames.size(), 1);
    EXPECT_EQ(result.frames[0].result, frame_send_result_e::complete);
    EXPECT_FALSE(result.frames[0].recovery_required);
  }

  TEST(TransportPacer, ReducingBurstOnlyChecksStillPendingPacketsAndNeverTheirSubmittedPredecessors) {
    pacer_bounds_t bounds;
    bounds.maximum_batch_packets = 1;
    deadline_pacer_t pacer(bounds);
    const auto handle = *pacer.add_session(7, limits(1000000, 3000), 0);
    auto encoded = frame(1, 1, 1, 2000, 1000000);
    auto small = frame(1, 2, 1, 1000, 1000000);
    encoded.packets.push_back(std::move(small.packets[0]));
    ASSERT_EQ(pacer.enqueue_frame(handle, std::move(encoded), 0).result, pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.dispatch(0, all_success(0)).successful.size(), 1);
    ASSERT_TRUE(pacer.update_limits(handle, limits(1000000, 1000), 0));
    auto remaining = pacer.dispatch(0, all_success(0));
    ASSERT_EQ(remaining.successful.size(), 1);
    ASSERT_EQ(remaining.frames.size(), 1);
    EXPECT_EQ(remaining.frames[0].result, frame_send_result_e::complete);
  }

  TEST(TransportPacer, TeardownStillReleasesQueuesWhenTheProvidedClockIsInvalid) {
    deadline_pacer_t pacer;
    const auto handle = *pacer.add_session(7, limits(), 100);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(1, 1, 1), 100).result, pacer_enqueue_result_e::queued);
    auto stopped = pacer.stop(-1);
    ASSERT_EQ(stopped.size(), 1);
    EXPECT_EQ(stopped[0].result, frame_send_result_e::stopped);
    EXPECT_EQ(pacer.host_snapshot().queued_payload_bytes, 0);
    EXPECT_EQ(pacer.host_snapshot().queued_packets, 0);
    EXPECT_TRUE(pacer.host_snapshot().stopped);
    EXPECT_FALSE(pacer.dispatch(101, all_success(101)).attempted);
  }

  TEST(TransportPacer, ExhaustedIdentityAndAlreadyExpiredFrameNeverReachTheSocket) {
    deadline_pacer_t pacer;
    const auto handle = *pacer.add_session(7, limits(), 0);
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    EXPECT_EQ(pacer.enqueue_frame(handle, frame(1, maximum, 1), 0).result, pacer_enqueue_result_e::invalid);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(1, maximum - 1, 1, 1200, 0, frame_dependency_e::reference), 0).result, pacer_enqueue_result_e::queued);
    auto expired = pacer.dispatch(0, [](auto, auto) {
      ADD_FAILURE() << "already expired frame cannot be sent";
      return paced_batch_submission_t {};
    });
    ASSERT_EQ(expired.frames.size(), 1);
    EXPECT_EQ(expired.frames[0].result, frame_send_result_e::deadline_expired);
    EXPECT_EQ(expired.frames[0].abandoned_packets, 1);
    EXPECT_TRUE(expired.frames[0].recovery_required);
    EXPECT_EQ(pacer.enqueue_frame(handle, frame(2, 1, 1), 0).result, pacer_enqueue_result_e::invalid);
  }

  TEST(TransportPacer, UnknownOsSubmissionClosesTheSharedOwnerAndReturnsEveryAbandonedFlow) {
    deadline_pacer_t pacer;
    ASSERT_TRUE(pacer.set_host_limits(limits(1000000, 10000), 0));
    const auto a = *pacer.add_session(7, limits(), 0);
    const auto b = *pacer.add_session(8, limits(), 0);
    ASSERT_EQ(pacer.enqueue_frame(a, frame(1, 1, 2), 0).result, pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.enqueue_frame(b, frame(1, 1, 2, 1200, 1000000, frame_dependency_e::reference, policy(8)), 0).result, pacer_enqueue_result_e::queued);
    auto failure = pacer.dispatch(0, [](auto, auto) -> paced_batch_submission_t {
      throw std::runtime_error("OS result unknowable after syscall");
    });
    EXPECT_TRUE(failure.sender_contract_broken);
    EXPECT_TRUE(failure.accounting_closed);
    EXPECT_FALSE(failure.next_wakeup_us);
    ASSERT_EQ(failure.frames.size(), 2);
    EXPECT_EQ(failure.frames[0].result, frame_send_result_e::sender_contract_broken);
    EXPECT_EQ(failure.frames[1].result, frame_send_result_e::stopped);
    EXPECT_FALSE(pacer.host_snapshot().accounting_valid);
    EXPECT_TRUE(pacer.snapshot(b)->stopped);
    EXPECT_TRUE(pacer.snapshot(b)->accounting_valid);
    EXPECT_EQ(pacer.host_snapshot().queued_payload_bytes, 0);
    EXPECT_EQ(pacer.host_snapshot().queued_packets, 0);
    EXPECT_FALSE(pacer.dispatch(1, all_success(1)).attempted);
  }

  TEST(TransportPacer, ThreeFlowActualSubmissionOracleVerifiesSharedHostBudgetWithHolesAndRateSteps) {
    pacer_bounds_t bounds;
    bounds.maximum_batch_packets = 4;
    bounds.batch_quantum_ip_bytes = 2400;
    deadline_pacer_t pacer(bounds);
    ASSERT_TRUE(pacer.set_host_limits(limits(100000, 3200, 1600), 0));
    std::array<std::uint64_t, 3> handles {};
    std::array<std::uint64_t, 3> next_frame { 1, 1, 1 }, next_sequence { 1, 1, 1 };
    std::array<send_ledger_t, 3> ledgers { send_ledger_t(7), send_ledger_t(8), send_ledger_t(9) };
    for (std::size_t flow = 0; flow < handles.size(); ++flow) handles[flow] = *pacer.add_session(7 + flow, limits(200000, 4000, 1200), 0);
    std::uint32_t random = 0x5086029U;
    auto draw = [&random]() {
      random ^= random << 13;
      random ^= random >> 17;
      random ^= random << 5;
      return random;
    };
    std::vector<std::tuple<std::int64_t, std::uint64_t, std::uint64_t>> actual;
    const std::vector<std::pair<std::int64_t, std::uint64_t>> host_rates { { 0, 100000 }, { 500000, 25000 }, { 1000000, 150000 } };
    std::size_t holes = 0;
    for (std::int64_t now = 0; now <= 2000000; now += 1000) {
      if (now == 500000) { ASSERT_TRUE(pacer.set_host_limits(limits(25000, 3200, 1600), now)); }
      if (now == 1000000) { ASSERT_TRUE(pacer.set_host_limits(limits(150000, 3200, 1600), now)); }
      for (std::size_t flow = 0; flow < handles.size(); ++flow) {
        if (pacer.snapshot(handles[flow])->queued_frames >= 2) continue;
        const auto count = static_cast<std::size_t>(2 + draw() % 4);
        const auto size = 100U + draw() % 1401;
        auto encoded = frame(next_frame[flow]++, next_sequence[flow], count, size, now + 300000,
          frame_dependency_e::non_reference, policy(7 + flow));
        next_sequence[flow] += count;
        ASSERT_EQ(pacer.enqueue_frame(handles[flow], std::move(encoded), now).result, pacer_enqueue_result_e::queued);
      }
      auto result = pacer.dispatch(now, [&actual, &draw, &holes, now](auto handle, auto packets) {
        paced_batch_submission_t value;
        value.completed_at_us = now;
        bool failed = false;
        for (const auto &packet : packets) {
          const bool submitted = draw() % 5 != 0;
          value.packets.push_back({ submitted, now });
          if (submitted) {
            actual.emplace_back(now, handle, packet.metadata.ip_bytes);
            holes += failed;
          }
          else
            failed = true;
        }
        return value;
      });
      ASSERT_FALSE(result.sender_contract_broken);
      ASSERT_FALSE(result.accounting_closed);
      for (const auto &submitted : result.successful) {
        const auto flow = static_cast<std::size_t>(std::find(handles.begin(), handles.end(), submitted.session_handle) - handles.begin());
        ASSERT_LT(flow, handles.size());
        ASSERT_TRUE(ledgers[flow].commit_success(submitted.packet));
      }
      ASSERT_LE(pacer.host_snapshot().budget_debt_bytes, 1600);
      ASSERT_LE(pacer.host_snapshot().instant_debt_bytes, 1600);
    }
    ASSERT_GT(actual.size(), 100);
    ASSERT_GT(holes, 0);
    std::uint64_t actual_total = 0;
    for (const auto &[time, handle, bytes] : actual) {
      (void) time;
      (void) handle;
      actual_total += bytes;
    }
    EXPECT_EQ(actual_total, pacer.host_snapshot().submitted_ip_bytes);
    for (std::size_t flow = 0; flow < handles.size(); ++flow) {
      std::uint64_t flow_bytes = 0;
      for (const auto &[time, handle, bytes] : actual) {
        (void) time;
        if (handle == handles[flow]) flow_bytes += bytes;
      }
      EXPECT_EQ(flow_bytes, pacer.snapshot(handles[flow])->submitted_ip_bytes);
      EXPECT_EQ(flow_bytes, ledgers[flow].snapshot().committed_ip_bytes);
      EXPECT_GT(ledgers[flow].snapshot().committed_packets, 20);
    }
    // Independent piecewise-rate oracle: every sampled interval contains
    // actual OS successes of all flows, including noncontiguous fallbacks.
    for (std::int64_t begin = 0; begin <= 2000000; begin += 20000) {
      for (std::int64_t end = begin; end <= 2000000; end += 20000) {
        std::uint64_t bytes_in_interval = 0, integral = 0;
        for (const auto &[time, handle, bytes] : actual) {
          (void) handle;
          if (time >= begin && time <= end) bytes_in_interval += bytes;
        }
        for (std::size_t i = 0; i < host_rates.size(); ++i) {
          const auto left = std::max(begin, host_rates[i].first);
          const auto right = std::min(end, i + 1 < host_rates.size() ? host_rates[i + 1].first : end);
          if (right > left) integral += static_cast<std::uint64_t>(right - left) * host_rates[i].second;
        }
        EXPECT_LE(bytes_in_interval * 1000000ULL, integral + 4800ULL * 1000000ULL)
          << "shared host interval [" << begin << "," << end << "]";
      }
    }
  }
  TEST(TransportPacerReferenceBreak, ExternalLossRetiresOwnedDependentsButInventsNoMissingFrameReceipt) {
    deadline_pacer_t pacer;
    const auto handle = *pacer.add_session(7, limits(1000000, 10000), 0);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(0, 1, 1, 1200, 1000000, frame_dependency_e::reference), 0).result, pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(2, 2, 1, 1200, 1000000, frame_dependency_e::reference), 0).result, pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(3, 3, 1), 0).result, pacer_enqueue_result_e::queued);
    auto marked = pacer.mark_reference_break(handle, 1, 0);
    EXPECT_EQ(marked.result, pacer_reference_break_result_e::marked);
    EXPECT_TRUE(marked.recovery_required);
    ASSERT_EQ(marked.frames.size(), 1);
    EXPECT_EQ(marked.frames[0].frame_id, 2);
    EXPECT_EQ(marked.frames[0].result, frame_send_result_e::reference_chain_broken);
    EXPECT_EQ(marked.frames[0].submitted_packets, 0);
    EXPECT_EQ(marked.frames[0].abandoned_packets, 1);
    EXPECT_EQ(marked.frames[0].abandoned_ip_bytes, 1200);
    EXPECT_EQ(pacer.snapshot(handle)->queued_payload_bytes, 2344);
    EXPECT_EQ(pacer.snapshot(handle)->queued_packets, 2);
    auto earlier = pacer.dispatch(0, all_success(0));
    ASSERT_EQ(earlier.successful.size(), 1);
    EXPECT_EQ(earlier.successful[0].packet.frame_id, 0);
    auto independent = pacer.dispatch(0, all_success(0));
    ASSERT_EQ(independent.successful.size(), 1);
    EXPECT_EQ(independent.successful[0].packet.frame_id, 3);
    EXPECT_TRUE(pacer.snapshot(handle)->reference_chain_broken);
  }

  TEST(TransportPacerReferenceBreak, PartialReferenceRetirementPreservesActualSuccessAndAllThreeDebts) {
    pacer_bounds_t bounds;
    bounds.maximum_batch_packets = 1;
    deadline_pacer_t pacer(bounds);
    auto configured = limits(1000, 0, 3600);
    configured.instant.rate_bytes_per_second = 2000;
    const auto handle = *pacer.add_session(7, configured, 0);
    ASSERT_TRUE(pacer.set_host_limits(limits(500, 0, 3600), 0));
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(10, 100, 3, 1000, 10000000, frame_dependency_e::reference), 0).result, pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(11, 103, 2, 600, 10000000, frame_dependency_e::reference), 0).result, pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.dispatch(0, all_success(0)).successful.size(), 1);
    EXPECT_EQ(pacer.snapshot(handle)->budget_debt_bytes, 1000);
    EXPECT_EQ(pacer.snapshot(handle)->instant_debt_bytes, 1000);
    EXPECT_EQ(pacer.host_snapshot().budget_debt_bytes, 1000);
    auto marked = pacer.mark_reference_break(handle, 10, 200000);
    ASSERT_EQ(marked.frames.size(), 2);
    EXPECT_EQ(marked.frames[0].frame_id, 10);
    EXPECT_EQ(marked.frames[0].submitted_packets, 1);
    EXPECT_EQ(marked.frames[0].submitted_ip_bytes, 1000);
    EXPECT_EQ(marked.frames[0].abandoned_packets, 2);
    EXPECT_EQ(marked.frames[0].abandoned_ip_bytes, 2000);
    EXPECT_EQ(marked.frames[1].frame_id, 11);
    EXPECT_EQ(marked.frames[1].abandoned_packets, 2);
    EXPECT_EQ(marked.frames[1].abandoned_ip_bytes, 1200);
    EXPECT_EQ(pacer.snapshot(handle)->submitted_ip_bytes, 1000);
    EXPECT_EQ(pacer.snapshot(handle)->queued_payload_bytes, 0);
    EXPECT_EQ(pacer.host_snapshot().queued_packets, 0);
    // Independent integral: 1000 B - {1000,2000,500} B/s * 0.2 s.
    EXPECT_EQ(pacer.snapshot(handle)->budget_debt_bytes, 800);
    EXPECT_EQ(pacer.snapshot(handle)->instant_debt_bytes, 600);
    EXPECT_EQ(pacer.host_snapshot().budget_debt_bytes, 900);
    EXPECT_EQ(pacer.host_snapshot().instant_debt_bytes, 900);
    EXPECT_FALSE(pacer.add_session(7, configured, 200000));
    EXPECT_EQ(pacer.enqueue_frame(handle, frame(12, 100, 1, 1000, 10000000, frame_dependency_e::recovery), 200000).result, pacer_enqueue_result_e::invalid);
    EXPECT_EQ(pacer.enqueue_frame(handle, frame(12, 105, 1, 1000, 10000000, frame_dependency_e::recovery, policy(8)), 200000).result, pacer_enqueue_result_e::invalid);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(12, 105, 1, 1000, 10000000, frame_dependency_e::recovery), 200000).result, pacer_enqueue_result_e::queued);
    auto restored = pacer.dispatch(200000, all_success(200000));
    ASSERT_EQ(restored.successful.size(), 1);
    EXPECT_EQ(restored.successful[0].session_handle, handle);
    EXPECT_EQ(restored.successful[0].policy->connection_epoch, 7);
    EXPECT_EQ(restored.successful[0].packet.extended_sequence, 105);
    EXPECT_FALSE(pacer.snapshot(handle)->reference_chain_broken);
    EXPECT_EQ(pacer.snapshot(handle)->budget_debt_bytes, 1800);
    EXPECT_EQ(pacer.snapshot(handle)->instant_debt_bytes, 1600);
    EXPECT_EQ(pacer.host_snapshot().budget_debt_bytes, 1900);
  }

  TEST(TransportPacerReferenceBreak, NewerQueuedRecoveryIsConditionalUntilEveryPacketCompletes) {
    pacer_bounds_t bounds;
    bounds.maximum_batch_packets = 1;
    deadline_pacer_t pacer(bounds);
    const auto handle = *pacer.add_session(7, limits(1000000, 10000), 0);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(2, 1, 1, 1200, 1000000, frame_dependency_e::reference), 0).result, pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(3, 2, 2, 1200, 1000000, frame_dependency_e::recovery), 0).result, pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(4, 4, 1, 1200, 1000000, frame_dependency_e::reference), 0).result, pacer_enqueue_result_e::queued);
    auto marked = pacer.mark_reference_break(handle, 1, 0);
    ASSERT_EQ(marked.frames.size(), 1);
    EXPECT_EQ(marked.frames[0].frame_id, 2);
    EXPECT_EQ(pacer.snapshot(handle)->queued_frames, 2);
    auto prefix = pacer.dispatch(0, all_success(0));
    ASSERT_EQ(prefix.successful.size(), 1);
    EXPECT_TRUE(prefix.frames.empty());
    EXPECT_TRUE(pacer.snapshot(handle)->reference_chain_broken);
    auto complete = pacer.dispatch(0, all_success(0));
    ASSERT_EQ(complete.frames.size(), 1);
    EXPECT_EQ(complete.frames[0].frame_id, 3);
    EXPECT_EQ(complete.frames[0].submitted_packets, 2);
    EXPECT_FALSE(complete.frames[0].recovery_required);
    auto dependent = pacer.dispatch(0, all_success(0));
    ASSERT_EQ(dependent.successful.size(), 1);
    EXPECT_EQ(dependent.successful[0].packet.frame_id, 4);
  }

  TEST(TransportPacerReferenceBreak, MarkingPartialRecoveryRetiresItsSuffixAndDependentReferences) {
    pacer_bounds_t bounds;
    bounds.maximum_batch_packets = 1;
    deadline_pacer_t pacer(bounds);
    const auto handle = *pacer.add_session(7, limits(1000000, 10000), 0);
    EXPECT_EQ(pacer.mark_reference_break(handle, 1, 0).result, pacer_reference_break_result_e::marked);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(3, 1, 2, 1200, 1000000, frame_dependency_e::recovery), 0).result, pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(4, 3, 1, 1200, 1000000, frame_dependency_e::reference), 0).result, pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(5, 4, 1), 0).result, pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.dispatch(0, all_success(0)).successful.size(), 1);
    auto lost = pacer.mark_reference_break(handle, 3, 0);
    EXPECT_EQ(lost.result, pacer_reference_break_result_e::marked);
    ASSERT_EQ(lost.frames.size(), 2);
    EXPECT_EQ(lost.frames[0].frame_id, 3);
    EXPECT_EQ(lost.frames[0].submitted_packets, 1);
    EXPECT_EQ(lost.frames[0].submitted_ip_bytes, 1200);
    EXPECT_EQ(lost.frames[0].abandoned_packets, 1);
    EXPECT_EQ(lost.frames[0].abandoned_ip_bytes, 1200);
    EXPECT_EQ(lost.frames[1].frame_id, 4);
    EXPECT_TRUE(pacer.snapshot(handle)->reference_chain_broken);
    auto independent = pacer.dispatch(0, all_success(0));
    ASSERT_EQ(independent.successful.size(), 1);
    EXPECT_EQ(independent.successful[0].packet.frame_id, 5);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(6, 5, 1, 1200, 1000000, frame_dependency_e::recovery), 0).result, pacer_enqueue_result_e::queued);
    auto recovered = pacer.dispatch(0, all_success(0));
    ASSERT_EQ(recovered.frames.size(), 1);
    EXPECT_FALSE(recovered.frames[0].recovery_required);
    EXPECT_EQ(pacer.snapshot(handle)->submitted_ip_bytes, 3600);
  }

  TEST(TransportPacerReferenceBreak, HigherBreakInvalidatesAnOlderBarrierWhilePreservingStrictlyNewerRecovery) {
    pacer_bounds_t bounds;
    bounds.maximum_batch_packets = 1;
    deadline_pacer_t pacer(bounds);
    const auto handle = *pacer.add_session(7, limits(1000000, 10000), 0);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(10, 1, 1, 1200, 1000000, frame_dependency_e::recovery), 0).result, pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(11, 2, 1, 1200, 1000000, frame_dependency_e::reference), 0).result, pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(12, 3, 2, 1200, 1000000, frame_dependency_e::recovery), 0).result, pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(13, 5, 1, 1200, 1000000, frame_dependency_e::reference), 0).result, pacer_enqueue_result_e::queued);
    auto first = pacer.mark_reference_break(handle, 9, 0);
    EXPECT_TRUE(first.frames.empty());
    auto newer = pacer.mark_reference_break(handle, 11, 0);
    EXPECT_EQ(newer.result, pacer_reference_break_result_e::marked);
    ASSERT_EQ(newer.frames.size(), 1);
    EXPECT_EQ(newer.frames[0].frame_id, 11);
    auto old_recovery = pacer.dispatch(0, all_success(0));
    ASSERT_EQ(old_recovery.frames.size(), 1);
    EXPECT_EQ(old_recovery.frames[0].frame_id, 10);
    EXPECT_TRUE(old_recovery.frames[0].recovery_required);
    EXPECT_TRUE(pacer.snapshot(handle)->reference_chain_broken);
    ASSERT_EQ(pacer.dispatch(0, all_success(0)).successful.size(), 1);
    EXPECT_TRUE(pacer.snapshot(handle)->reference_chain_broken);
    auto full = pacer.dispatch(0, all_success(0));
    ASSERT_EQ(full.frames.size(), 1);
    EXPECT_FALSE(full.frames[0].recovery_required);
    auto dependent = pacer.dispatch(0, all_success(0));
    ASSERT_EQ(dependent.successful.size(), 1);
    EXPECT_EQ(dependent.successful[0].packet.frame_id, 13);
  }

  TEST(TransportPacerReferenceBreak, RecoveryOlderThanAFutureIngressLossCannotClearIt) {
    deadline_pacer_t pacer;
    const auto handle = *pacer.add_session(7, limits(1000000, 10000), 0);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(5, 1, 1, 1200, 1000000, frame_dependency_e::recovery), 0).result, pacer_enqueue_result_e::queued);
    auto marked = pacer.mark_reference_break(handle, 20, 0);
    EXPECT_TRUE(marked.frames.empty());
    auto older = pacer.dispatch(0, all_success(0));
    ASSERT_EQ(older.frames.size(), 1);
    EXPECT_TRUE(older.frames[0].recovery_required);
    EXPECT_EQ(pacer.mark_reference_break(handle, 4, 0).result, pacer_reference_break_result_e::stale);
    EXPECT_TRUE(pacer.snapshot(handle)->reference_chain_broken);
    EXPECT_EQ(pacer.enqueue_frame(handle, frame(20, 2, 1), 0).result, pacer_enqueue_result_e::invalid);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(21, 2, 1, 1200, 1000000, frame_dependency_e::recovery), 0).result, pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.dispatch(0, all_success(0)).successful.size(), 1);
    EXPECT_FALSE(pacer.snapshot(handle)->reference_chain_broken);
  }

  TEST(TransportPacerReferenceBreak, RepeatedMarkerDoesNotDuplicateSettlementsOrForgiveDebt) {
    deadline_pacer_t pacer;
    const auto handle = *pacer.add_session(7, limits(1000, 0, 2400), 0);
    ASSERT_TRUE(pacer.external_allowance(handle, 1200, 0));
    ASSERT_TRUE(pacer.debit_external_success(handle, 1200, 0));
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(2, 1, 1, 1200, 10000000, frame_dependency_e::reference), 0).result, pacer_enqueue_result_e::queued);
    auto first = pacer.mark_reference_break(handle, 1, 0);
    ASSERT_EQ(first.frames.size(), 1);
    auto repeated = pacer.mark_reference_break(handle, 1, 200000);
    EXPECT_EQ(repeated.result, pacer_reference_break_result_e::already_broken);
    EXPECT_TRUE(repeated.frames.empty());
    EXPECT_EQ(pacer.snapshot(handle)->budget_debt_bytes, 1000);
    EXPECT_EQ(pacer.snapshot(handle)->externally_submitted_ip_bytes, 1200);
    EXPECT_EQ(pacer.snapshot(handle)->submitted_ip_bytes, 0);
    auto higher = pacer.mark_reference_break(handle, 9, 200000);
    EXPECT_EQ(higher.result, pacer_reference_break_result_e::marked);
    EXPECT_TRUE(higher.frames.empty());
    EXPECT_EQ(pacer.enqueue_frame(handle, frame(9, 2, 1), 200000).result, pacer_enqueue_result_e::invalid);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(10, 2, 1, 1200, 10000000, frame_dependency_e::recovery), 200000).result, pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.dispatch(200000, all_success(200000)).successful.size(), 1);
    EXPECT_EQ(pacer.snapshot(handle)->budget_debt_bytes, 2200);
    EXPECT_FALSE(pacer.snapshot(handle)->reference_chain_broken);
  }

  TEST(TransportPacerReferenceBreak, FrameZeroHasNoImaginaryCompletedRecovery) {
    deadline_pacer_t pacer;
    const auto handle = *pacer.add_session(7, limits(), 0);
    auto zero = pacer.mark_reference_break(handle, 0, 0);
    EXPECT_EQ(zero.result, pacer_reference_break_result_e::marked);
    EXPECT_TRUE(zero.recovery_required);
    EXPECT_EQ(pacer.enqueue_frame(handle, frame(0, 1, 1), 0).result, pacer_enqueue_result_e::invalid);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(1, 1, 1, 1200, 1000000, frame_dependency_e::recovery), 0).result, pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.dispatch(0, all_success(0)).successful.size(), 1);
    EXPECT_EQ(pacer.mark_reference_break(handle, 0, 0).result, pacer_reference_break_result_e::stale);
    EXPECT_FALSE(pacer.snapshot(handle)->reference_chain_broken);
  }

  TEST(TransportPacerReferenceBreak, LateMarkerAfterCompletedRecoveryIsEntirelyStale) {
    deadline_pacer_t pacer;
    const auto handle = *pacer.add_session(7, limits(1000, 0, 2400), 0);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(10, 1, 1, 1200, 10000000, frame_dependency_e::recovery), 0).result, pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.dispatch(0, all_success(0)).successful.size(), 1);
    auto stale = pacer.mark_reference_break(handle, 8, 200000);
    EXPECT_EQ(stale.result, pacer_reference_break_result_e::stale);
    EXPECT_FALSE(stale.recovery_required);
    EXPECT_TRUE(stale.frames.empty());
    EXPECT_EQ(pacer.snapshot(handle)->budget_debt_bytes, 1200);
    // A rejected stale call cannot advance the global clock or consume ID 11.
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(11, 2, 1), 0).result, pacer_enqueue_result_e::queued);
    EXPECT_FALSE(pacer.snapshot(handle)->reference_chain_broken);
  }

  TEST(TransportPacerReferenceBreak, OldMarkerBeforeRecoveryMayRetireNewerQueuedFrames) {
    deadline_pacer_t pacer;
    const auto handle = *pacer.add_session(7, limits(1000000, 10000), 0);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(4, 1, 1, 1200, 1000000, frame_dependency_e::reference), 0).result, pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(5, 2, 1), 0).result, pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(6, 3, 1, 1200, 1000000, frame_dependency_e::recovery), 0).result, pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(7, 4, 1, 1200, 1000000, frame_dependency_e::reference), 0).result, pacer_enqueue_result_e::queued);
    auto old = pacer.mark_reference_break(handle, 3, 0);
    EXPECT_EQ(old.result, pacer_reference_break_result_e::marked);
    ASSERT_EQ(old.frames.size(), 1);
    EXPECT_EQ(old.frames[0].frame_id, 4);
    auto independent = pacer.dispatch(0, all_success(0));
    ASSERT_EQ(independent.successful.size(), 1);
    EXPECT_EQ(independent.successful[0].packet.frame_id, 5);
    auto recovery = pacer.dispatch(0, all_success(0));
    ASSERT_EQ(recovery.successful.size(), 1);
    EXPECT_EQ(recovery.successful[0].packet.frame_id, 6);
    auto dependent = pacer.dispatch(0, all_success(0));
    ASSERT_EQ(dependent.successful.size(), 1);
    EXPECT_EQ(dependent.successful[0].packet.frame_id, 7);
  }

  TEST(TransportPacerReferenceBreak, UnknownStoppedAndInvalidClockCallsDoNotMutateOwner) {
    deadline_pacer_t pacer;
    const auto handle = *pacer.add_session(7, limits(1000, 0, 2400), 100);
    ASSERT_TRUE(pacer.debit_external_success(handle, 900, 100));
    EXPECT_EQ(pacer.mark_reference_break(999, 20, 1000000).result, pacer_reference_break_result_e::unknown_session);
    EXPECT_EQ(pacer.mark_reference_break(handle, 20, -1).result, pacer_reference_break_result_e::clock_invalid);
    EXPECT_EQ(pacer.mark_reference_break(handle, 20, 99).result, pacer_reference_break_result_e::clock_invalid);
    EXPECT_FALSE(pacer.snapshot(handle)->reference_chain_broken);
    EXPECT_EQ(pacer.snapshot(handle)->budget_debt_bytes, 900);
    EXPECT_EQ(pacer.mark_reference_break(handle, 1, 100).result, pacer_reference_break_result_e::marked);
    EXPECT_EQ(pacer.snapshot(handle)->budget_debt_bytes, 900);
    EXPECT_TRUE(pacer.stop_session(handle, 100).empty());
    EXPECT_EQ(pacer.mark_reference_break(handle, 2, 100).result, pacer_reference_break_result_e::unknown_session);
    const auto next = *pacer.add_session(8, limits(), 100);
    EXPECT_NE(next, handle);
    pacer.stop(100);
    EXPECT_EQ(pacer.mark_reference_break(next, 3, 100).result, pacer_reference_break_result_e::stopped);
    EXPECT_FALSE(pacer.snapshot(next)->reference_chain_broken);
  }

  TEST(TransportPacerReferenceBreak, SenderCallbackCannotReenterOrConsumeFutureFrameIdentity) {
    deadline_pacer_t pacer;
    const auto handle = *pacer.add_session(7, limits(1000000, 10000), 0);
    EXPECT_EQ(pacer.mark_reference_break(handle, 1, 0).result, pacer_reference_break_result_e::marked);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(2, 1, 1, 1200, 1000000, frame_dependency_e::recovery), 0).result, pacer_enqueue_result_e::queued);
    auto recovery = pacer.dispatch(0, [&](auto, auto packets) {
      auto rejected = pacer.mark_reference_break(handle, 100, 0);
      EXPECT_EQ(rejected.result, pacer_reference_break_result_e::clock_invalid);
      EXPECT_TRUE(rejected.frames.empty());
      return paced_batch_submission_t { std::vector<packet_submission_t>(packets.size(), { true, 0 }), 0, true };
    });
    ASSERT_EQ(recovery.frames.size(), 1);
    EXPECT_FALSE(recovery.frames[0].recovery_required);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(3, 2, 1, 1200, 1000000, frame_dependency_e::reference), 0).result, pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.dispatch(0, all_success(0)).successful.size(), 1);
  }

  TEST(TransportPacerReferenceBreak, ExpiredRecoveryCannotShieldDependentQueuedReferences) {
    deadline_pacer_t pacer;
    const auto handle = *pacer.add_session(7, limits(), 0);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(3, 1, 1, 1200, 10, frame_dependency_e::recovery), 0).result, pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(4, 2, 1, 1200, 1000000, frame_dependency_e::reference), 0).result, pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(5, 3, 1), 0).result, pacer_enqueue_result_e::queued);
    auto marked = pacer.mark_reference_break(handle, 1, 10);
    ASSERT_EQ(marked.frames.size(), 2);
    EXPECT_EQ(marked.frames[0].frame_id, 3);
    EXPECT_EQ(marked.frames[0].result, frame_send_result_e::deadline_expired);
    EXPECT_EQ(marked.frames[1].frame_id, 4);
    EXPECT_EQ(marked.frames[1].result, frame_send_result_e::reference_chain_broken);
    auto surviving = pacer.dispatch(10, all_success(10));
    ASSERT_EQ(surviving.successful.size(), 1);
    EXPECT_EQ(surviving.successful[0].packet.frame_id, 5);
    EXPECT_TRUE(pacer.snapshot(handle)->reference_chain_broken);
  }

  TEST(TransportPacerReferenceBreak, FailedRecoveryBarrierRetiresReferencesOnNextDispatch) {
    deadline_pacer_t pacer;
    const auto handle = *pacer.add_session(7, limits(), 0);
    EXPECT_EQ(pacer.mark_reference_break(handle, 1, 0).result, pacer_reference_break_result_e::marked);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(3, 1, 1, 1200, 1000000, frame_dependency_e::recovery), 0).result, pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(4, 2, 1, 1200, 1000000, frame_dependency_e::reference), 0).result, pacer_enqueue_result_e::queued);
    auto failed = pacer.dispatch(0, [](auto, auto) { return paced_batch_submission_t { { { false, 0 } }, 0, false }; });
    ASSERT_EQ(failed.frames.size(), 1);
    EXPECT_EQ(failed.frames[0].result, frame_send_result_e::send_failed);
    EXPECT_EQ(failed.frames[0].submitted_packets, 0);
    EXPECT_TRUE(failed.frames[0].recovery_required);
    auto dependent = pacer.dispatch(0, all_success(0));
    EXPECT_FALSE(dependent.attempted);
    ASSERT_EQ(dependent.frames.size(), 1);
    EXPECT_EQ(dependent.frames[0].frame_id, 4);
    EXPECT_EQ(dependent.frames[0].result, frame_send_result_e::reference_chain_broken);
    EXPECT_EQ(pacer.snapshot(handle)->submitted_ip_bytes, 0);
  }

  TEST(TransportPacerReferenceBreak, RetiredOwnersFreeBoundsWithoutResettingReservedSequence) {
    pacer_bounds_t bounds;
    bounds.maximum_frames_per_session = 2;
    bounds.maximum_queued_packets = 2;
    bounds.maximum_queued_payload_bytes = 1944;
    deadline_pacer_t pacer(bounds);
    const auto handle = *pacer.add_session(7, limits(1000000, 10000), 0);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(1, 1, 1, 1000, 1000000, frame_dependency_e::reference), 0).result, pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(2, 2, 1, 1000, 1000000, frame_dependency_e::reference), 0).result, pacer_enqueue_result_e::queued);
    auto marked = pacer.mark_reference_break(handle, 1, 0);
    ASSERT_EQ(marked.frames.size(), 2);
    EXPECT_EQ(pacer.host_snapshot().queued_payload_bytes, 0);
    EXPECT_EQ(pacer.host_snapshot().queued_packets, 0);
    EXPECT_EQ(pacer.enqueue_frame(handle, frame(3, 2, 2, 1000, 1000000, frame_dependency_e::recovery), 0).result, pacer_enqueue_result_e::invalid);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(3, 3, 2, 1000, 1000000, frame_dependency_e::recovery), 0).result, pacer_enqueue_result_e::queued);
    EXPECT_EQ(pacer.host_snapshot().queued_payload_bytes, 1944);
    auto result = pacer.dispatch(0, all_success(0));
    ASSERT_EQ(result.successful.size(), 2);
    EXPECT_EQ(result.successful[0].packet.extended_sequence, 3);
    EXPECT_EQ(result.successful[1].packet.extended_sequence, 4);
    EXPECT_FALSE(pacer.snapshot(handle)->reference_chain_broken);
  }

  TEST(TransportPacerReferenceBreak, AnotherSessionRetainsOwnershipCreditAndSchedulingTurn) {
    deadline_pacer_t pacer;
    const auto a = *pacer.add_session(7, limits(0, 0, 2400), 0);
    const auto b = *pacer.add_session(8, limits(0, 0, 2400), 0);
    ASSERT_EQ(pacer.enqueue_frame(a, frame(1, 1, 1, 1200, 1000000, frame_dependency_e::reference), 0).result, pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.enqueue_frame(b, frame(1, 1, 1, 1200, 1000000, frame_dependency_e::reference, policy(8)), 0).result, pacer_enqueue_result_e::queued);
    auto marked = pacer.mark_reference_break(a, 1, 0);
    ASSERT_EQ(marked.frames.size(), 1);
    EXPECT_EQ(pacer.snapshot(b)->queued_payload_bytes, 1172);
    EXPECT_EQ(pacer.snapshot(b)->budget_debt_bytes, 0);
    EXPECT_FALSE(pacer.snapshot(b)->reference_chain_broken);
    auto other = pacer.dispatch(0, all_success(0));
    ASSERT_EQ(other.successful.size(), 1);
    EXPECT_EQ(other.successful[0].session_handle, b);
    EXPECT_EQ(pacer.snapshot(b)->budget_debt_bytes, 1200);
    EXPECT_EQ(pacer.snapshot(a)->budget_debt_bytes, 0);
    EXPECT_EQ(pacer.host_snapshot().submitted_ip_bytes, 1200);
  }

  TEST(TransportPacerReferenceBreak, MarkerCannotReviveAnOwnerClosedByUnknownSubmission) {
    deadline_pacer_t pacer;
    const auto handle = *pacer.add_session(7, limits(), 0);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(1, 1, 1, 1200, 1000000, frame_dependency_e::reference), 0).result, pacer_enqueue_result_e::queued);
    auto unknowable = pacer.dispatch(0, [](auto, auto) { return paced_batch_submission_t { {}, 0, false }; });
    ASSERT_TRUE(unknowable.accounting_closed);
    ASSERT_TRUE(unknowable.sender_contract_broken);
    auto marked = pacer.mark_reference_break(handle, 2, 1000000);
    EXPECT_EQ(marked.result, pacer_reference_break_result_e::stopped);
    EXPECT_TRUE(marked.frames.empty());
    EXPECT_FALSE(pacer.snapshot(handle)->accounting_valid);
    EXPECT_TRUE(pacer.snapshot(handle)->stopped);
    EXPECT_EQ(pacer.snapshot(handle)->submitted_ip_bytes, 0);
    EXPECT_EQ(pacer.host_snapshot().queued_packets, 0);
    EXPECT_FALSE(pacer.add_session(8, limits(), 1000000));
  }
  TEST(TransportPacer, RecoveryPrimaryFitsDeadlineEvenWhenParityDoesNot) {
    deadline_pacer_t pacer;
    const auto handle = *pacer.add_session(7, limits(1000000, 100), 0);
    ASSERT_EQ(pacer.mark_reference_break(handle, 0, 0).result, pacer_reference_break_result_e::marked);
    ASSERT_EQ(pacer.enqueue_frame(handle, primary_then_fec(1, 1, 2, 2, 150, frame_dependency_e::recovery), 0).result,
      pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.enqueue_frame(handle, primary_then_fec(2, 5, 1, 0, 500, frame_dependency_e::reference), 0).result,
      pacer_enqueue_result_e::queued);

    for (const auto now : { 0, 100 }) {
      const auto sent = pacer.dispatch(now, all_success(now));
      ASSERT_EQ(sent.successful.size(), 1);
      EXPECT_EQ(sent.successful[0].packet.kind, packet_kind_e::data);
      EXPECT_EQ(sent.successful[0].packet.extended_sequence, now == 0 ? 1u : 2u);
      EXPECT_TRUE(sent.frames.empty());
    }
    const auto retired = pacer.dispatch(101, all_success(101));
    ASSERT_EQ(retired.frames.size(), 1);
    EXPECT_EQ(retired.frames[0].result, frame_send_result_e::cannot_meet_deadline);
    EXPECT_TRUE(retired.frames[0].primary_complete);
    EXPECT_FALSE(retired.frames[0].recovery_required);
    EXPECT_EQ(retired.frames[0].submitted_ip_bytes, 200u);
    EXPECT_EQ(retired.frames[0].abandoned_ip_bytes, 200u);
    EXPECT_TRUE(retired.successful.empty());
    const auto next = pacer.dispatch(200, all_success(200));
    ASSERT_EQ(next.frames.size(), 1);
    EXPECT_EQ(next.frames[0].result, frame_send_result_e::complete);
    EXPECT_EQ(next.successful[0].packet.extended_sequence, 5u);
    EXPECT_EQ(pacer.host_snapshot().submitted_ip_bytes, 300u);
    EXPECT_EQ(pacer.snapshot(handle)->budget_debt_bytes, 0u);
    EXPECT_FALSE(pacer.snapshot(handle)->reference_chain_broken);
  }

  TEST(TransportPacer, InterleavedParityStillCountsTowardPrimaryCompletionDeadline) {
    deadline_pacer_t pacer;
    const auto handle = *pacer.add_session(7, limits(1000000, 100), 0);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(1, 1, 4, 100, 150, frame_dependency_e::recovery), 0).result,
      pacer_enqueue_result_e::queued);
    const auto retired = pacer.dispatch(0, [](auto, auto) {
      ADD_FAILURE() << "the source prefix including interleaved parity cannot meet its deadline";
      return paced_batch_submission_t {};
    });
    ASSERT_EQ(retired.frames.size(), 1);
    EXPECT_EQ(retired.frames[0].result, frame_send_result_e::cannot_meet_deadline);
    EXPECT_FALSE(retired.frames[0].primary_complete);
    EXPECT_TRUE(retired.frames[0].recovery_required);
    EXPECT_EQ(retired.frames[0].submitted_ip_bytes, 0u);
    EXPECT_EQ(retired.frames[0].abandoned_ip_bytes, 400u);
  }

  TEST(TransportPacer, UnfundableParityPacketDoesNotRejectFundablePrimary) {
    deadline_pacer_t pacer;
    const auto handle = *pacer.add_session(7, limits(1000000, 100), 0);
    auto value = primary_then_fec(1, 1, 1, 1, 1000, frame_dependency_e::reference);
    value.packets[1].udp_payload.resize(300 - 28);
    value.packets[1].metadata.ip_bytes = 300;
    ASSERT_EQ(pacer.enqueue_frame(handle, std::move(value), 0).result, pacer_enqueue_result_e::queued);
    const auto sent = pacer.dispatch(0, all_success(0));
    ASSERT_EQ(sent.successful.size(), 1);
    EXPECT_EQ(sent.successful[0].packet.kind, packet_kind_e::data);
    const auto retired = pacer.dispatch(1, all_success(1));
    ASSERT_EQ(retired.frames.size(), 1);
    EXPECT_EQ(retired.frames[0].result, frame_send_result_e::cannot_meet_deadline);
    EXPECT_TRUE(retired.frames[0].primary_complete);
    EXPECT_FALSE(retired.frames[0].recovery_required);
    EXPECT_EQ(retired.frames[0].abandoned_ip_bytes, 300u);
    EXPECT_EQ(pacer.host_snapshot().submitted_ip_bytes, 100u);
  }

  TEST(TransportPacer, TimelyPrimaryWithExpiredParityDoesNotBreakTheNextReference) {
    deadline_pacer_t pacer;
    const auto handle = *pacer.add_session(7, limits(1000000, 100), 0);
    ASSERT_EQ(pacer.enqueue_frame(handle, primary_then_fec(1, 1, 1, 1, 200, frame_dependency_e::reference), 0).result,
      pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.enqueue_frame(handle, primary_then_fec(2, 3, 1, 0, 1000, frame_dependency_e::reference), 0).result,
      pacer_enqueue_result_e::queued);
    const auto first = pacer.dispatch(0, all_success(0));
    ASSERT_EQ(first.successful.size(), 1);
    EXPECT_EQ(first.successful[0].deadline_us, 200);
    EXPECT_TRUE(first.frames.empty());
    const auto retired = pacer.dispatch(200, all_success(200));
    ASSERT_EQ(retired.frames.size(), 2);
    EXPECT_EQ(retired.frames[0].result, frame_send_result_e::deadline_expired);
    EXPECT_TRUE(retired.frames[0].primary_complete);
    EXPECT_FALSE(retired.frames[0].recovery_required);
    EXPECT_EQ(retired.frames[0].abandoned_packets, 1);
    EXPECT_EQ(retired.frames[0].abandoned_ip_bytes, 100);
    EXPECT_EQ(retired.frames[1].result, frame_send_result_e::complete);
    ASSERT_EQ(retired.successful.size(), 1);
    EXPECT_EQ(retired.successful[0].packet.extended_sequence, 3u);
    EXPECT_EQ(retired.successful[0].deadline_us, 1000);
    EXPECT_EQ(pacer.snapshot(handle)->submitted_ip_bytes, 200u);
    EXPECT_FALSE(pacer.snapshot(handle)->reference_chain_broken);
  }

  TEST(TransportPacer, TimelyRecoveryPrimaryWithExpiredParityClearsOnlyTheOldBreak) {
    deadline_pacer_t pacer;
    const auto handle = *pacer.add_session(7, limits(1000000, 100), 0);
    ASSERT_EQ(pacer.mark_reference_break(handle, 0, 0).result, pacer_reference_break_result_e::marked);
    ASSERT_EQ(pacer.enqueue_frame(handle, primary_then_fec(1, 1, 1, 1, 200, frame_dependency_e::recovery), 0).result,
      pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.enqueue_frame(handle, primary_then_fec(2, 3, 1, 0, 1000, frame_dependency_e::reference), 0).result,
      pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.dispatch(0, all_success(0)).successful.size(), 1);
    EXPECT_TRUE(pacer.snapshot(handle)->reference_chain_broken);
    const auto retired = pacer.dispatch(200, all_success(200));
    ASSERT_EQ(retired.frames.size(), 2);
    EXPECT_EQ(retired.frames[0].result, frame_send_result_e::deadline_expired);
    EXPECT_TRUE(retired.frames[0].primary_complete);
    EXPECT_FALSE(retired.frames[0].recovery_required);
    EXPECT_EQ(retired.frames[1].result, frame_send_result_e::complete);
    EXPECT_FALSE(pacer.snapshot(handle)->reference_chain_broken);
  }

  TEST(TransportPacer, BudgetReductionCanAbandonOnlyParityWithoutForgivingDebtOrBreakingReference) {
    deadline_pacer_t pacer;
    const auto handle = *pacer.add_session(7, limits(1000000, 100), 0);
    ASSERT_EQ(pacer.enqueue_frame(handle, primary_then_fec(1, 1, 1, 1, 200, frame_dependency_e::reference), 0).result,
      pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.dispatch(0, all_success(0)).successful.size(), 1);
    ASSERT_TRUE(pacer.update_limits(handle, limits(0, 0), 1));
    const auto retired = pacer.dispatch(1, [](auto, auto) {
      ADD_FAILURE() << "unfunded parity must not reach the socket";
      return paced_batch_submission_t {};
    });
    ASSERT_EQ(retired.frames.size(), 1);
    EXPECT_EQ(retired.frames[0].result, frame_send_result_e::cannot_meet_deadline);
    EXPECT_TRUE(retired.frames[0].primary_complete);
    EXPECT_FALSE(retired.frames[0].recovery_required);
    EXPECT_EQ(retired.frames[0].submitted_ip_bytes, 100u);
    EXPECT_EQ(retired.frames[0].abandoned_ip_bytes, 100u);
    EXPECT_EQ(pacer.host_snapshot().submitted_ip_bytes, 100u);
  }

  TEST(TransportPacer, LateParityIsStillAccountedButDoesNotEraseTimelyRecoveryPrimary) {
    deadline_pacer_t pacer;
    const auto handle = *pacer.add_session(7, limits(1000000, 200), 0);
    ASSERT_EQ(pacer.mark_reference_break(handle, 0, 0).result, pacer_reference_break_result_e::marked);
    ASSERT_EQ(pacer.enqueue_frame(handle, primary_then_fec(1, 1, 1, 1, 10, frame_dependency_e::recovery), 0).result,
      pacer_enqueue_result_e::queued);
    const auto result = pacer.dispatch(0, [](auto, auto) {
      return paced_batch_submission_t { { { true, 9 }, { true, 10 } }, 11, true };
    });
    ASSERT_EQ(result.frames.size(), 1);
    EXPECT_EQ(result.frames[0].result, frame_send_result_e::deadline_expired);
    EXPECT_TRUE(result.frames[0].primary_complete);
    EXPECT_FALSE(result.frames[0].recovery_required);
    EXPECT_EQ(result.successful.size(), 2);
    EXPECT_EQ(pacer.snapshot(handle)->submitted_ip_bytes, 200u);
  }

  TEST(TransportPacer, LatePrimaryCannotRepairAReferenceEvenWithAllParitySubmitted) {
    deadline_pacer_t pacer;
    const auto handle = *pacer.add_session(7, limits(1000000, 200), 0);
    ASSERT_EQ(pacer.enqueue_frame(handle, primary_then_fec(1, 1, 1, 1, 10, frame_dependency_e::recovery), 0).result,
      pacer_enqueue_result_e::queued);
    const auto result = pacer.dispatch(0, [](auto, auto) {
      return paced_batch_submission_t { { { true, 10 }, { true, 11 } }, 12, true };
    });
    ASSERT_EQ(result.frames.size(), 1);
    EXPECT_FALSE(result.frames[0].primary_complete);
    EXPECT_TRUE(result.frames[0].recovery_required);
    EXPECT_EQ(result.frames[0].result, frame_send_result_e::deadline_expired);
    EXPECT_EQ(pacer.snapshot(handle)->submitted_ip_bytes, 200u);
  }

  TEST(TransportPacer, KnownParityHolesDoNotRetryIdentitiesOrBreakCompletePrimary) {
    deadline_pacer_t pacer;
    const auto handle = *pacer.add_session(7, limits(1000000, 400), 0);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(1, 1, 4, 100, 1000, frame_dependency_e::reference), 0).result,
      pacer_enqueue_result_e::queued);
    const auto result = pacer.dispatch(0, [](auto, auto) {
      return paced_batch_submission_t { { { true, 1 }, { false, 0 }, { true, 3 }, { false, 0 } }, 4, true };
    });
    ASSERT_EQ(result.frames.size(), 1);
    EXPECT_TRUE(result.frames[0].primary_complete);
    EXPECT_FALSE(result.frames[0].recovery_required);
    EXPECT_EQ(result.frames[0].result, frame_send_result_e::send_failed);
    EXPECT_EQ(result.frames[0].abandoned_packets, 2);
    EXPECT_EQ(result.frames[0].abandoned_ip_bytes, 200u);
    ASSERT_EQ(pacer.enqueue_frame(handle, primary_then_fec(2, 5, 1, 0, 1000, frame_dependency_e::reference), 4).result,
      pacer_enqueue_result_e::queued);
    const auto next = pacer.dispatch(4, all_success(4));
    ASSERT_EQ(next.successful.size(), 1);
    EXPECT_EQ(next.successful[0].packet.extended_sequence, 5u);
  }

  TEST(TransportPacer, MissingPrimaryCannotBeCountedCompleteJustBecauseParitySucceeded) {
    deadline_pacer_t pacer;
    const auto handle = *pacer.add_session(7, limits(1000000, 300), 0);
    ASSERT_EQ(pacer.enqueue_frame(handle, primary_then_fec(1, 1, 2, 1, 1000, frame_dependency_e::reference), 0).result,
      pacer_enqueue_result_e::queued);
    const auto result = pacer.dispatch(0, [](auto, auto) {
      return paced_batch_submission_t { { { false, 0 }, { true, 2 }, { true, 3 } }, 4, true };
    });
    ASSERT_EQ(result.frames.size(), 1);
    EXPECT_FALSE(result.frames[0].primary_complete);
    EXPECT_TRUE(result.frames[0].recovery_required);
    EXPECT_EQ(result.successful.size(), 2);
    EXPECT_EQ(result.frames[0].abandoned_packets, 1);
  }

  TEST(TransportPacer, UnknownSuffixNeverClearsRecoveryEvenAfterAConfirmedPrimaryPrefix) {
    deadline_pacer_t pacer;
    const auto handle = *pacer.add_session(7, limits(1000000, 200), 0);
    ASSERT_EQ(pacer.mark_reference_break(handle, 0, 0).result, pacer_reference_break_result_e::marked);
    ASSERT_EQ(pacer.enqueue_frame(handle, primary_then_fec(1, 1, 1, 1, 1000, frame_dependency_e::recovery), 0).result,
      pacer_enqueue_result_e::queued);
    const auto result = pacer.dispatch(0, [](auto, auto) {
      return paced_batch_submission_t { { { true, 1 }, { false, 0 } }, 2, false, false };
    });
    ASSERT_TRUE(result.accounting_closed);
    ASSERT_EQ(result.successful.size(), 1);
    ASSERT_EQ(result.frames.size(), 1);
    EXPECT_FALSE(result.frames[0].primary_complete);
    EXPECT_TRUE(result.frames[0].recovery_required);
    EXPECT_FALSE(pacer.snapshot(handle)->accounting_valid);
    EXPECT_EQ(pacer.snapshot(handle)->submitted_ip_bytes, 100u);
  }

  TEST(TransportPacer, AnExplicitReferenceLossStillRetiresTimelyPrimaryAndPendingParity) {
    deadline_pacer_t pacer;
    const auto handle = *pacer.add_session(7, limits(1000000, 100), 0);
    ASSERT_EQ(pacer.enqueue_frame(handle, primary_then_fec(1, 1, 1, 1, 200, frame_dependency_e::recovery), 0).result,
      pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.dispatch(0, all_success(0)).successful.size(), 1);
    const auto marked = pacer.mark_reference_break(handle, 1, 1);
    ASSERT_EQ(marked.frames.size(), 1);
    EXPECT_EQ(marked.frames[0].result, frame_send_result_e::reference_chain_broken);
    EXPECT_FALSE(marked.frames[0].primary_complete);
    EXPECT_TRUE(marked.frames[0].recovery_required);
    EXPECT_EQ(marked.frames[0].abandoned_packets, 1);
    EXPECT_EQ(pacer.snapshot(handle)->submitted_ip_bytes, 100u);
  }

  TEST(TransportPacer, CompleteRecoveryPrimaryCannotClearANewerIngressBreak) {
    deadline_pacer_t pacer;
    const auto handle = *pacer.add_session(7, limits(1000000, 100), 0);
    ASSERT_EQ(pacer.enqueue_frame(handle, primary_then_fec(1, 1, 1, 1, 200, frame_dependency_e::recovery), 0).result,
      pacer_enqueue_result_e::queued);
    ASSERT_EQ(pacer.mark_reference_break(handle, 3, 0).result, pacer_reference_break_result_e::marked);
    ASSERT_EQ(pacer.dispatch(0, all_success(0)).successful.size(), 1);
    const auto retired = pacer.dispatch(200, all_success(200));
    ASSERT_EQ(retired.frames.size(), 1);
    EXPECT_TRUE(retired.frames[0].primary_complete);
    EXPECT_TRUE(retired.frames[0].recovery_required);
    ASSERT_EQ(pacer.enqueue_frame(handle, primary_then_fec(4, 3, 1, 0, 1000, frame_dependency_e::recovery), 200).result,
      pacer_enqueue_result_e::queued);
    const auto recovered = pacer.dispatch(200, all_success(200));
    ASSERT_EQ(recovered.frames.size(), 1);
    EXPECT_TRUE(recovered.frames[0].primary_complete);
    EXPECT_FALSE(recovered.frames[0].recovery_required);
  }

  TEST(TransportPacer, ReferenceRecoveryRequiresRealPrimaryDataRatherThanParityOrProbes) {
    deadline_pacer_t pacer;
    const auto handle = *pacer.add_session(7, limits(), 0);
    for (const auto kind : { packet_kind_e::fec, packet_kind_e::probe, packet_kind_e::repair, static_cast<packet_kind_e>(99) }) {
      auto invalid = frame(1, 1, 1, 100, 1000, frame_dependency_e::recovery);
      invalid.packets[0].metadata.kind = kind;
      EXPECT_EQ(pacer.enqueue_frame(handle, std::move(invalid), 0).result, pacer_enqueue_result_e::invalid);
    }
    auto invalid_dependency = frame(1, 1, 1, 100, 1000, static_cast<frame_dependency_e>(99));
    EXPECT_EQ(pacer.enqueue_frame(handle, std::move(invalid_dependency), 0).result, pacer_enqueue_result_e::invalid);
    ASSERT_EQ(pacer.enqueue_frame(handle, primary_then_fec(1, 1, 1, 0, 1000, frame_dependency_e::recovery), 0).result,
      pacer_enqueue_result_e::queued);
    EXPECT_TRUE(pacer.dispatch(0, all_success(0)).frames[0].primary_complete);
  }

  TEST(TransportPacer, EmergencyAbortReleasesOwnedFramesPreservesKnownSendsAndDebt) {
    deadline_pacer_t pacer;
    ASSERT_TRUE(pacer.set_host_limits(limits(0, 0, 2400), 0));
    const auto a = *pacer.add_session(7, limits(0, 0, 2400), 0);
    const auto b = *pacer.add_session(8, limits(), 0);
    ASSERT_EQ(pacer.enqueue_frame(a, frame(1, 1, 1), 0).result, pacer_enqueue_result_e::queued);
    auto sent = pacer.dispatch(0, all_success(0));
    ASSERT_EQ(sent.successful.size(), 1);
    auto owned_policy = policy(8, 42);
    std::weak_ptr<const frame_policy_t> weak = owned_policy;
    ASSERT_EQ(pacer.enqueue_frame(b, frame(1, 1, 2, 1200, 1000000, frame_dependency_e::reference, owned_policy), 0).result,
      pacer_enqueue_result_e::queued);
    owned_policy.reset();
    ASSERT_FALSE(weak.expired());
    ASSERT_TRUE(pacer.abort_noexcept());
    EXPECT_TRUE(weak.expired());
    EXPECT_EQ(pacer.snapshot(a)->submitted_ip_bytes, 1200);
    EXPECT_EQ(pacer.snapshot(a)->budget_debt_bytes, 1200);
    EXPECT_FALSE(pacer.snapshot(a)->accounting_valid);
    EXPECT_TRUE(pacer.snapshot(a)->stopped);
    EXPECT_EQ(pacer.snapshot(b)->queued_frames, 0);
    EXPECT_EQ(pacer.snapshot(b)->queued_packets, 0);
    EXPECT_EQ(pacer.snapshot(b)->queued_payload_bytes, 0);
    EXPECT_FALSE(pacer.snapshot(b)->accounting_valid);
    EXPECT_EQ(pacer.host_snapshot().submitted_ip_bytes, 1200);
    EXPECT_EQ(pacer.host_snapshot().budget_debt_bytes, 1200);
    EXPECT_EQ(pacer.host_snapshot().queued_packets, 0);
    EXPECT_EQ(pacer.host_snapshot().queued_payload_bytes, 0);
    bool called = false;
    EXPECT_FALSE(pacer.dispatch(1000000, [&](auto, auto) { called = true; return paced_batch_submission_t {}; }).attempted);
    EXPECT_FALSE(called);
    EXPECT_FALSE(pacer.add_session(9, limits(), 1000000));
    EXPECT_TRUE(pacer.abort_noexcept());
  }

  TEST(TransportPacer, EmergencyAbortRejectsSenderReentryWithoutInvalidatingViews) {
    deadline_pacer_t pacer;
    const auto handle = *pacer.add_session(7, limits(), 0);
    ASSERT_EQ(pacer.enqueue_frame(handle, frame(1, 1, 1), 0).result, pacer_enqueue_result_e::queued);
    auto sent = pacer.dispatch(0, [&](auto, auto packets) {
      EXPECT_FALSE(pacer.abort_noexcept());
      EXPECT_EQ(packets[0].udp_payload.size(), 1172);
      return paced_batch_submission_t { { { true, 0 } }, 0, false };
    });
    EXPECT_EQ(sent.successful.size(), 1);
    EXPECT_TRUE(pacer.snapshot(handle)->accounting_valid);
    EXPECT_FALSE(pacer.snapshot(handle)->stopped);
  }
}  // namespace
