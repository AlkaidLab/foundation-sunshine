#include "src/transport/transport_feedback.h"

#include <array>
#include <limits>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

namespace {
  using namespace transport;

  sent_packet_t packet(std::uint64_t sequence, std::int64_t time = 100) {
    return {sequence, time, 1280, 37, 12, packet_kind_e::fec, {}};
  }

  packet_observation_t received(std::uint64_t sequence, std::int64_t time = 9000000) {
    return {sequence, packet_status_e::received, time};
  }

  packet_observation_t missing(std::uint64_t sequence) {
    return {sequence, packet_status_e::missing, -1};
  }

  TEST(NetworkWindow, SuccessfulSetExcludesUnsentHolesAndTracksUnknownTail) {
    send_ledger_t ledger(7);
    ASSERT_TRUE(ledger.commit_success(packet(100, 1000000)));
    ASSERT_TRUE(ledger.commit_success(packet(102, 1000010)));
    ASSERT_TRUE(ledger.commit_success(packet(104, 1000020)));
    const std::array rows {received(100, 1000000), missing(101), missing(102)};
    ASSERT_EQ(ledger.apply({7,1,1,1100000,rows}).result, report_result_e::accepted);
    const auto view = ledger.network_window(1300000);
    EXPECT_TRUE(view.valid);
    EXPECT_FALSE(view.history_truncated);
    EXPECT_EQ(view.received, 1u);
    EXPECT_EQ(view.missing, 1u);
    EXPECT_EQ(view.unknown, 1u);
    EXPECT_EQ(view.connection_epoch, 7u);
    EXPECT_EQ(view.receiver_clock_epoch, 1u);
  }

  TEST(NetworkWindow, MaturityAndWindowBoundariesExcludeUnsettledRecentPackets) {
    send_ledger_t ledger(7);
    for (const auto &[id,time] : std::array<std::pair<uint64_t,int64_t>,4>{{{1,999999},{2,1000000},{3,2800000},{4,2800001}}})
      ASSERT_TRUE(ledger.commit_success(packet(id,time)));
    const auto view = ledger.network_window(3000000);
    EXPECT_TRUE(view.valid);
    EXPECT_EQ(view.begins_at_us, 1000000);
    EXPECT_EQ(view.ends_at_us, 2800000);
    EXPECT_EQ(view.unknown, 2u);
  }

  TEST(NetworkWindow, CapacityEvictionAndSamplingCapInvalidateWholeCoverage) {
    send_ledger_t ledger(7,2);
    for (uint64_t id=1; id<=3; ++id) ASSERT_TRUE(ledger.commit_success(packet(id,1000000)));
    const auto evicted = ledger.network_window(1300000);
    EXPECT_TRUE(evicted.history_truncated);
    EXPECT_EQ(evicted.unknown, 2u);
    const auto capped = ledger.network_window(1300000,2000000,200000,1);
    EXPECT_TRUE(capped.history_truncated);
    EXPECT_EQ(capped.unknown, 1u);
    // Old evictions cannot invalidate a later, empty observation window forever.
    EXPECT_FALSE(ledger.network_window(4000000).history_truncated);
  }

  TEST(NetworkWindow, LateCorrectionChangesCurrentMissingWithoutDoubleCounting) {
    send_ledger_t ledger(7);
    ASSERT_TRUE(ledger.commit_success(packet(1,1000000)));
    const std::array lost {missing(1)};
    ASSERT_EQ(ledger.apply({7,1,1,1100000,lost}).result, report_result_e::accepted);
    EXPECT_EQ(ledger.network_window(1300000).missing, 1u);
    const std::array arrived {received(1,1200000)};
    ASSERT_EQ(ledger.apply({7,2,1,1250000,arrived}).result, report_result_e::accepted);
    auto view = ledger.network_window(1300000);
    EXPECT_EQ(view.received, 1u);
    EXPECT_EQ(view.missing, 0u);
    EXPECT_EQ(ledger.snapshot().missing_declarations, 1u);
    EXPECT_EQ(ledger.snapshot().late_corrections, 1u);
  }

  TEST(NetworkWindow, NewReceiverClockMakesEarlierCoverageUnknown) {
    send_ledger_t ledger(7);
    ASSERT_TRUE(ledger.commit_success(packet(1,1000000)));
    const std::array arrived {received(1,1050000)};
    ASSERT_EQ(ledger.apply({7,1,1,1100000,arrived}).result, report_result_e::accepted);
    EXPECT_EQ(ledger.network_window(1300000).received, 1u);
    const std::array unmatched {received(2,1150000)};
    ASSERT_EQ(ledger.apply({7,2,2,1200000,unmatched}).result, report_result_e::accepted);
    const auto view = ledger.network_window(1300000);
    EXPECT_EQ(view.received, 0u);
    EXPECT_EQ(view.unknown, 1u);
    EXPECT_EQ(view.receiver_clock_epoch, 2u);
  }

  TEST(NetworkWindow, InvalidWindowInputsDoNotProduceValidEmptyCoverage) {
    send_ledger_t ledger(7);
    EXPECT_FALSE(ledger.network_window(-1).valid);
    EXPECT_FALSE(ledger.network_window(100).valid);
    EXPECT_FALSE(ledger.network_window(3000000,0).valid);
    EXPECT_FALSE(ledger.network_window(3000000,2000000,2000000).valid);
    EXPECT_FALSE(ledger.network_window(3000000,2000000,200000,0).valid);
    EXPECT_FALSE(ledger.network_window(3000000,2000000,200000,32769).valid);
  }

  TEST(NetworkWindow, OptInHistoryPreservesTheWholeMatureWindowAtHighPacketRates) {
    for (const int rate : {1000,5000,15000,30000}) {
      send_ledger_t ledger(7,65536);
      std::vector<packet_observation_t> rows;
      uint64_t report_sequence = 0;
      int64_t sent_at = 0;
      const auto flush = [&] {
        const auto result = ledger.apply({7,++report_sequence,1,sent_at+10000,rows});
        ASSERT_EQ(result.result,report_result_e::accepted);
        rows.clear();
      };
      for (int i=0; i<rate*2; ++i) {
        sent_at = 1000000 + static_cast<int64_t>(i)*1000000/rate;
        const auto id = static_cast<uint64_t>(i)+1;
        ASSERT_TRUE(ledger.commit_success(packet(id,sent_at)));
        rows.push_back(id%100 == 0 ? missing(id) : received(id,sent_at+10000));
        if (rows.size() == 256) flush();
      }
      if (!rows.empty()) flush();
      const auto window = ledger.network_window(3000000);
      ASSERT_TRUE(window.valid);
      EXPECT_EQ(window.unknown,0u);
      if (rate <= 15000) {
        const auto expected = static_cast<uint32_t>(rate*18/10+1);
        EXPECT_FALSE(window.history_truncated);
        EXPECT_EQ(window.received+window.missing,expected);
        EXPECT_EQ(window.missing,expected/100);
      } else {
        EXPECT_TRUE(window.history_truncated);
        EXPECT_EQ(window.received+window.missing,32768u);
      }
    }
  }

  feedback_result_t report(send_ledger_t &ledger, std::uint64_t sequence,
                           std::span<const packet_observation_t> observations,
                           std::uint64_t clock_epoch = 1, std::int64_t time = 1000) {
    return ledger.apply({7, sequence, clock_epoch, time, observations});
  }

  TEST(TransportFeedback, OnlySuccessfulSubmissionsDefineTheDenominator) {
    send_ledger_t ledger(7);
    ASSERT_TRUE(ledger.commit_success(packet(65534)));
    ASSERT_TRUE(ledger.commit_success(packet(65536)));
    const std::array observations {received(65534), missing(65535), received(65536)};
    const auto result = report(ledger, 1, observations);
    ASSERT_EQ(result.result, report_result_e::accepted);
    ASSERT_EQ(result.changes.size(), 2);
    EXPECT_EQ(result.unmatched_packets, 1);
    EXPECT_EQ(ledger.snapshot().committed_packets, 2);
    EXPECT_EQ(ledger.snapshot().received_packets, 2);
    EXPECT_EQ(ledger.snapshot().missing_declarations, 0);
    EXPECT_EQ(ledger.snapshot().data_in_flight_bytes, 0);
    EXPECT_EQ(result.changes.front().sent.frame_id, 37);
    EXPECT_EQ(result.changes.front().sent.policy_revision, 12);
    EXPECT_EQ(result.changes.front().sent.kind, packet_kind_e::fec);
  }

  TEST(TransportFeedback, MissingAndLateReceptionAreEachDeliveredOnce) {
    send_ledger_t ledger(7);
    ASSERT_TRUE(ledger.commit_success(packet(10)));
    const std::array lost {missing(10)};
    auto result = report(ledger, 1, lost);
    ASSERT_EQ(result.changes.size(), 1);
    EXPECT_TRUE(result.changes.front().first_missing);
    EXPECT_EQ(ledger.snapshot().data_in_flight_bytes, 0);
    EXPECT_TRUE(report(ledger, 2, lost).changes.empty());
    const std::array arrived {received(10)};
    result = report(ledger, 3, arrived);
    ASSERT_EQ(result.changes.size(), 1);
    EXPECT_TRUE(result.changes.front().first_recovered);
    EXPECT_EQ(ledger.snapshot().received_ip_bytes, 1280);
    EXPECT_EQ(ledger.snapshot().late_corrections, 1);
    EXPECT_TRUE(report(ledger, 4, arrived).changes.empty());
    EXPECT_TRUE(report(ledger, 5, lost).changes.empty());
    EXPECT_EQ(ledger.snapshot().missing_declarations, 1);
    EXPECT_EQ(ledger.snapshot().received_packets, 1);
    EXPECT_EQ(ledger.snapshot().data_in_flight_bytes, 0);
  }

  TEST(TransportFeedback, PendingAndUnknownNeverBecomeLoss) {
    send_ledger_t ledger(7);
    ASSERT_TRUE(ledger.commit_success(packet(1)));
    ASSERT_TRUE(ledger.commit_success(packet(2)));
    const std::array observations {
      packet_observation_t {1, packet_status_e::pending, -1},
      packet_observation_t {2, packet_status_e::unknown, -1}};
    EXPECT_TRUE(report(ledger, 1, observations).changes.empty());
    EXPECT_EQ(ledger.snapshot().missing_declarations, 0);
    EXPECT_EQ(ledger.snapshot().data_in_flight_bytes, 2560);
    ledger.expire_before(101);
    EXPECT_EQ(ledger.size(), 0);
    EXPECT_EQ(ledger.snapshot().unresolved_evictions, 2);
    EXPECT_EQ(ledger.snapshot().missing_declarations, 0);
    EXPECT_EQ(ledger.snapshot().data_in_flight_bytes, 0);
  }

  TEST(TransportFeedback, HistoryPressureAndExpiredTailsStayUnknown) {
    send_ledger_t ledger(7, 2);
    ASSERT_TRUE(ledger.commit_success(packet(1, 100)));
    ASSERT_TRUE(ledger.commit_success(packet(2, 101)));
    ASSERT_TRUE(ledger.commit_success(packet(3, 102)));
    EXPECT_FALSE(ledger.find(1));
    EXPECT_EQ(ledger.snapshot().unresolved_evictions, 1);
    const std::array observations {missing(1), received(2)};
    EXPECT_EQ(report(ledger, 1, observations).unmatched_packets, 1);
    EXPECT_EQ(ledger.snapshot().missing_declarations, 0);
    ledger.expire_before(102);
    EXPECT_EQ(ledger.size(), 1);
    EXPECT_EQ(ledger.snapshot().unresolved_evictions, 1);
    EXPECT_EQ(ledger.snapshot().data_in_flight_bytes, 1280);
  }

  TEST(TransportFeedback, NewInformationInAnOlderReportIsAccepted) {
    send_ledger_t ledger(7);
    ASSERT_TRUE(ledger.commit_success(packet(1)));
    ASSERT_TRUE(ledger.commit_success(packet(2)));
    const std::array second {received(2)};
    EXPECT_EQ(report(ledger, 1000, second).changes.size(), 1);
    const std::array first {received(1)};
    EXPECT_EQ(report(ledger, 999, first).changes.size(), 1);
    EXPECT_EQ(report(ledger, 999, first).result, report_result_e::duplicate);
    EXPECT_EQ(report(ledger, 488, first).result, report_result_e::too_old);
    EXPECT_EQ(report(ledger, 489, first).result, report_result_e::accepted);
    EXPECT_EQ(ledger.snapshot().received_packets, 2);
  }

  TEST(TransportFeedback, ReportReplayWindowIsBoundedAfterLargeJump) {
    send_ledger_t ledger(7);
    EXPECT_EQ(report(ledger, 1, {}).result, report_result_e::accepted);
    EXPECT_EQ(report(ledger, std::numeric_limits<std::uint64_t>::max(), {}).result, report_result_e::accepted);
    EXPECT_EQ(report(ledger, 1, {}).result, report_result_e::too_old);
    EXPECT_EQ(report(ledger, std::numeric_limits<std::uint64_t>::max(), {}).result, report_result_e::duplicate);
  }

  TEST(TransportFeedback, MalformedReportIsAtomicAndDoesNotConsumeItsSequence) {
    send_ledger_t ledger(7);
    ASSERT_TRUE(ledger.commit_success(packet(1)));
    ASSERT_TRUE(ledger.commit_success(packet(2)));
    std::array observations {received(1), missing(2)};
    observations[1].first_arrival_us = 42;
    EXPECT_EQ(report(ledger, 1, observations).result, report_result_e::invalid);
    EXPECT_EQ(ledger.snapshot().received_packets, 0);
    EXPECT_EQ(ledger.snapshot().data_in_flight_bytes, 2560);
    observations[1].first_arrival_us = -1;
    EXPECT_EQ(report(ledger, 1, observations).result, report_result_e::accepted);
    EXPECT_EQ(ledger.snapshot().received_packets, 1);
    EXPECT_EQ(ledger.snapshot().missing_declarations, 1);
  }

  TEST(TransportFeedback, DuplicateRangesAndInvalidEnumsAreRejected) {
    send_ledger_t ledger(7);
    const std::array duplicate {received(1), received(1)};
    EXPECT_EQ(report(ledger, 1, duplicate).result, report_result_e::invalid);
    const std::array unsorted {received(2), received(1)};
    EXPECT_EQ(report(ledger, 1, unsorted).result, report_result_e::invalid);
    const std::array invalid {packet_observation_t {1, static_cast<packet_status_e>(255), -1}};
    EXPECT_EQ(report(ledger, 1, invalid).result, report_result_e::invalid);
    std::vector<packet_observation_t> too_large(send_ledger_t::max_report_packets + 1);
    EXPECT_EQ(report(ledger, 1, too_large).result, report_result_e::invalid);
  }

  TEST(TransportFeedback, FirstArrivalCannotBeRewrittenByARepeatedReport) {
    send_ledger_t ledger(7);
    ASSERT_TRUE(ledger.commit_success(packet(1)));
    const std::array first {received(1, 999)};
    EXPECT_EQ(report(ledger, 1, first).result, report_result_e::accepted);
    const std::array changed {received(1, 1000)};
    EXPECT_EQ(report(ledger, 2, changed).result, report_result_e::invalid);
    EXPECT_EQ(report(ledger, 2, first).result, report_result_e::accepted);
    EXPECT_EQ(ledger.snapshot().received_packets, 1);
  }

  TEST(TransportFeedback, EpochChangesRequireEstimatorResetButDoNotDuplicateBytes) {
    send_ledger_t ledger(7);
    ASSERT_TRUE(ledger.commit_success(packet(1)));
    const std::array first {received(1, 9000000)};
    EXPECT_FALSE(report(ledger, 1, first).receiver_clock_changed);
    const std::array changed_clock {received(1, 5)};
    auto result = report(ledger, 2, changed_clock, 2);
    EXPECT_EQ(result.result, report_result_e::accepted);
    EXPECT_TRUE(result.receiver_clock_changed);
    EXPECT_TRUE(result.changes.empty());
    EXPECT_EQ(report(ledger, 3, first, 1).result, report_result_e::wrong_epoch);
    EXPECT_EQ(ledger.apply({8, 4, 2, 1000, first}).result, report_result_e::wrong_epoch);
    EXPECT_EQ(ledger.snapshot().received_ip_bytes, 1280);
  }

  TEST(TransportFeedback, ReceiverSampleAndEpochOrderRejectInconsistentReports) {
    send_ledger_t ledger(7);
    ASSERT_TRUE(ledger.commit_success(packet(1)));
    const std::array observations {received(1, 9000100)};
    EXPECT_EQ(ledger.apply({7, 5, 1, 1000, observations, 9000000}).result, report_result_e::invalid);
    EXPECT_EQ(ledger.apply({7, 5, 1, 1000, observations, 9000200}).result, report_result_e::accepted);
    EXPECT_EQ(ledger.apply({7, 4, 2, 1000, {}, 10}).result, report_result_e::invalid);
    EXPECT_EQ(ledger.apply({7, 6, 2, 1000, {}, 10}).result, report_result_e::accepted);
    EXPECT_EQ(ledger.snapshot().received_packets, 1);
  }

  TEST(TransportFeedback, InvalidCommitAndClockValuesDoNotEnterTheLedger) {
    EXPECT_THROW(send_ledger_t(0), std::invalid_argument);
    EXPECT_THROW(send_ledger_t(7, 0), std::invalid_argument);
    EXPECT_THROW(send_ledger_t(7, send_ledger_t::max_capacity + 1), std::invalid_argument);
    send_ledger_t ledger(7);
    auto invalid = packet(1, -1);
    EXPECT_FALSE(ledger.commit_success(invalid));
    invalid = packet(1);
    invalid.ip_bytes = 65576;
    EXPECT_FALSE(ledger.commit_success(invalid));
    invalid = packet(1);
    invalid.kind = static_cast<packet_kind_e>(255);
    EXPECT_FALSE(ledger.commit_success(invalid));
    ASSERT_TRUE(ledger.commit_success(packet(1)));
    EXPECT_FALSE(ledger.commit_success(packet(1)));
    EXPECT_FALSE(ledger.commit_success(packet(0)));
    EXPECT_FALSE(ledger.commit_success(packet(2, 99)));
    const std::array observation {received(1)};
    EXPECT_EQ(report(ledger, 1, observation, 1, 99).result, report_result_e::invalid);
    EXPECT_EQ(report(ledger, 1, observation, 0).result, report_result_e::invalid);
    EXPECT_EQ(report(ledger, 0, observation).result, report_result_e::invalid);
    EXPECT_EQ(report(ledger, 1, observation).result, report_result_e::accepted);
    EXPECT_EQ(report(ledger, 2, {}, 1, 999).result, report_result_e::invalid);
  }

  TEST(TransportFeedback, DeterministicIndependentLedgerMatchesAcrossSequenceWraps) {
    send_ledger_t ledger(7, 256);
    std::uint64_t expected_committed = 0, expected_received = 0, expected_missing = 0;
    std::uint64_t expected_late = 0, report_sequence = 0;
    for (std::uint64_t batch = 0; batch < 1200; ++batch) {
      std::vector<packet_observation_t> observations, late;
      for (std::uint64_t index = batch * 128; index < (batch + 1) * 128; ++index) {
        // Every 23rd planned transmission fails before submission. Independent
        // receiver ranges still include it, so it must never become network loss.
        const bool submitted = index % 23 != 0;
        if (submitted) {
          ASSERT_TRUE(ledger.commit_success(packet(index, static_cast<std::int64_t>(index))));
          ++expected_committed;
        }
        if (index % 19 == 0) {
          observations.push_back({index, packet_status_e::unknown, -1});
        }
        else if (index % 17 == 0) {
          observations.push_back(missing(index));
          if (submitted) {
            ++expected_missing;
          }
          if (index % 5 == 0) {
            late.push_back(received(index, 5000000 + static_cast<std::int64_t>(index)));
            if (submitted) {
              ++expected_received;
              ++expected_late;
            }
          }
        }
        else {
          observations.push_back(received(index, 5000000 + static_cast<std::int64_t>(index)));
          if (submitted) {
            ++expected_received;
          }
        }
      }
      const auto time = static_cast<std::int64_t>((batch + 1) * 128);
      EXPECT_EQ(report(ledger, ++report_sequence, observations, 1, time).result, report_result_e::accepted);
      EXPECT_EQ(report(ledger, report_sequence, observations, 1, time).result, report_result_e::duplicate);
      EXPECT_EQ(report(ledger, ++report_sequence, late, 1, time).result, report_result_e::accepted);
    }
    const auto &snapshot = ledger.snapshot();
    EXPECT_EQ(snapshot.committed_packets, expected_committed);
    EXPECT_EQ(snapshot.received_packets, expected_received);
    EXPECT_EQ(snapshot.committed_ip_bytes, expected_committed * 1280);
    EXPECT_EQ(snapshot.received_ip_bytes, expected_received * 1280);
    EXPECT_EQ(snapshot.missing_declarations, expected_missing);
    EXPECT_EQ(snapshot.late_corrections, expected_late);
    EXPECT_TRUE(snapshot.counters_valid);
    EXPECT_LE(ledger.size(), 256);
    EXPECT_LE(snapshot.data_in_flight_bytes, 256 * 1280);
  }
}  // namespace
