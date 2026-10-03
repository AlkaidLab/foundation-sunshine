#include "src/transport_feedback_wire.h"

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>

namespace {
  TF_PACKET_REPORT
  make_report(unsigned count = 4) {
    TF_PACKET_REPORT r {};
    r.connectionEpoch = 42;
    r.reportSequence = 1;
    r.receiverClockEpoch = 1;
    r.receiverSampleTimeUs = 100000;
    r.baseExtendedSequence = 100;
    r.packetCount = static_cast<uint16_t>(count);
    for (unsigned i = 0; i < count; ++i) {
      r.status[i] = i % 4;
      r.firstArrivalTimeUs[i] = r.status[i] == TF_RECEIVED ? r.receiverSampleTimeUs - i : TF_NO_ARRIVAL;
    }
    return r;
  }

  std::vector<uint8_t>
  encode(const TF_PACKET_REPORT &r) {
    std::vector<uint8_t> bytes(TF_MAX_REPORT_BYTES);
    const auto count = TfEncodeReport(&r, bytes.data(), bytes.size());
    EXPECT_NE(count, 0u);
    bytes.resize(count);
    return bytes;
  }

  transport::sent_packet_t
  sent(uint64_t sequence) {
    return { sequence, static_cast<int64_t>(sequence), 1234, sequence / 10, 1, transport::packet_kind_e::data, {} };
  }
}  // namespace

TEST(TransportFeedbackWire, IndependentGoldenBytesPreserveHeaderStatusesAndFirstArrival) {
  auto r = make_report();
  r.connectionEpoch = 0x0102030405060708ull;
  r.reportSequence = 9;
  r.receiverClockEpoch = 11;
  r.receiverSampleTimeUs = 0x01000002;
  r.baseExtendedSequence = UINT64_MAX - 3;
  r.firstArrivalTimeUs[1] = r.receiverSampleTimeUs - 10;
  const std::vector<uint8_t> golden {
    0, 1, 0, 56, 0, 1, 0, 0,
    1, 2, 3, 4, 5, 6, 7, 8,
    0, 0, 0, 0, 0, 0, 0, 9,
    0, 0, 0, 0, 0, 0, 0, 11,
    0, 0, 0, 0, 1, 0, 0, 2,
    255, 255, 255, 255, 255, 255, 255, 252,
    0, 4, 0, 0, 0xe4, 0, 0, 10
  };
  EXPECT_EQ(encode(r), golden);
  TF_PACKET_REPORT decoded {};
  ASSERT_TRUE(TfDecodeReport(golden.data(), golden.size(), &decoded));
  EXPECT_EQ(decoded.baseExtendedSequence, UINT64_MAX - 3);
  EXPECT_EQ(decoded.firstArrivalTimeUs[1], 0x00fffff8u);
  EXPECT_EQ(decoded.firstArrivalTimeUs[0], TF_NO_ARRIVAL);
  EXPECT_EQ(decoded.connectionEpoch, r.connectionEpoch);
}

TEST(NetworkStatisticsWire, NoChangeHeartbeatCannotRefreshNewCoverage) {
  transport::wire_feedback_t wire(42,true);
  ASSERT_TRUE(wire.commit_success({100,1000000,1234,1,1,transport::packet_kind_e::data,{}}));
  auto report = make_report(1);
  report.status[0] = TF_RECEIVED;
  report.firstArrivalTimeUs[0] = report.receiverSampleTimeUs;
  ASSERT_EQ(wire.apply_wire(encode(report),1100000).result,transport::report_result_e::accepted);
  ++report.reportSequence;
  ASSERT_EQ(wire.apply_wire(encode(report),2100000).result,transport::report_result_e::accepted);
  const auto statistics = wire.network_statistics(2300000);
  EXPECT_EQ(statistics.cumulative.last_feedback_us,2100000);
  EXPECT_EQ(statistics.cumulative.last_new_feedback_us,1100000);
  EXPECT_EQ(statistics.cumulative.latest_covered_send_us,1000000);
  EXPECT_EQ(statistics.window.received,1u);
}

TEST(NetworkStatisticsWire, UnmatchedAndOldCoveredSendsDoNotGrantFreshCoverage) {
  transport::wire_feedback_t wire(42,true);
  ASSERT_TRUE(wire.commit_success({100,1000000,1234,1,1,transport::packet_kind_e::data,{}}));
  auto report = make_report(1);
  report.status[0] = TF_RECEIVED;
  report.firstArrivalTimeUs[0] = report.receiverSampleTimeUs;
  ASSERT_EQ(wire.apply_wire(encode(report),2100000).result,transport::report_result_e::accepted);
  report.baseExtendedSequence = 101;
  ++report.reportSequence;
  ASSERT_EQ(wire.apply_wire(encode(report),2200000).result,transport::report_result_e::accepted);
  const auto statistics = wire.network_statistics(2300000);
  EXPECT_EQ(statistics.cumulative.last_new_feedback_us,2100000);
  EXPECT_EQ(statistics.cumulative.latest_covered_send_us,1000000);
  EXPECT_EQ(statistics.cumulative.ledger.unmatched_observations,1u);
}

TEST(NetworkStatisticsWire, ReceiverClockResetRevokesPreviousFreshness) {
  transport::wire_feedback_t wire(42,true);
  ASSERT_TRUE(wire.commit_success({100,1000000,1234,1,1,transport::packet_kind_e::data,{}}));
  auto report = make_report(1);
  report.status[0] = TF_RECEIVED;
  report.firstArrivalTimeUs[0] = report.receiverSampleTimeUs;
  ASSERT_EQ(wire.apply_wire(encode(report),1100000).result,transport::report_result_e::accepted);
  ++report.reportSequence;
  ++report.receiverClockEpoch;
  ASSERT_EQ(wire.apply_wire(encode(report),1200000).result,transport::report_result_e::accepted);
  const auto statistics = wire.network_statistics(1300000);
  EXPECT_EQ(statistics.window.receiver_clock_epoch,2u);
  EXPECT_EQ(statistics.window.unknown,1u);
  EXPECT_EQ(statistics.cumulative.last_new_feedback_us,-1);
  EXPECT_EQ(statistics.cumulative.latest_covered_send_us,-1);
}

TEST(TransportFeedbackWire, EveryPacketCountAndStatusRoundTripsWithoutArrivalOrderAssumption) {
  for (unsigned n = 1; n <= TF_MAX_PACKETS; ++n) {
    for (unsigned phase = 0; phase < 5; ++phase) {
      auto r = make_report(n);
      for (unsigned i = 0; i < n; ++i) {
        r.status[i] = phase < 4 ? phase : i % 4;
        // Earlier sequence can arrive after a later one.
        r.firstArrivalTimeUs[i] = r.status[i] == TF_RECEIVED ? r.receiverSampleTimeUs - ((i * 73) % 1000) : TF_NO_ARRIVAL;
      }
      const auto bytes = encode(r);
      TF_PACKET_REPORT decoded {};
      ASSERT_TRUE(TfDecodeReport(bytes.data(), bytes.size(), &decoded));
      EXPECT_EQ(decoded.packetCount, n);
      for (unsigned i = 0; i < n; ++i) {
        EXPECT_EQ(decoded.status[i], r.status[i]);
        EXPECT_EQ(decoded.firstArrivalTimeUs[i], r.firstArrivalTimeUs[i]);
      }
      EXPECT_LE(bytes.size(), TF_MAX_REPORT_BYTES);
      // Bound covers UDP/IPv6, ENet and authenticated control framing.
      EXPECT_LE(bytes.size() + transport::wire_feedback_t::report_wire_overhead_bound, 1200u);
    }
  }
}

TEST(TransportFeedbackWire, TruncationExtraBytesAndInvalidHeaderAreAtomic) {
  const auto bytes = encode(make_report(7));
  TF_PACKET_REPORT destination;
  std::memset(&destination, 0xa5, sizeof(destination));
  const auto original = destination;
  for (size_t length = 0; length < bytes.size(); ++length) {
    EXPECT_FALSE(TfDecodeReport(bytes.data(), length, &destination));
    EXPECT_EQ(std::memcmp(&destination, &original, sizeof(destination)), 0);
  }
  auto extra = bytes;
  extra.push_back(0);
  EXPECT_FALSE(TfDecodeReport(extra.data(), extra.size(), &destination));
  for (const auto offset : { 0u, 2u, 4u, 6u, 50u }) {
    auto invalid = bytes;
    invalid[offset] = 1;
    EXPECT_FALSE(TfDecodeReport(invalid.data(), invalid.size(), &destination)) << offset;
  }
  auto padding = bytes;
  padding[TF_REPORT_HEADER_BYTES + 1] |= 0xc0;
  EXPECT_FALSE(TfDecodeReport(padding.data(), padding.size(), &destination));
  EXPECT_EQ(std::memcmp(&destination, &original, sizeof(destination)), 0);
}

TEST(TransportFeedbackWire, InvalidEncodeNeverWritesOrTruncatesNumbers) {
  std::array<uint8_t, TF_MAX_REPORT_BYTES> output;
  output.fill(0xab);
  const auto unchanged = output;
  auto invalid = make_report();
  const auto reject = [&](const TF_PACKET_REPORT &r) {
    EXPECT_EQ(TfEncodeReport(&r, output.data(), output.size()), 0u);
    EXPECT_EQ(output, unchanged);
  };
  invalid.connectionEpoch = 0;
  reject(invalid);
  invalid = make_report();
  invalid.reportSequence = 0;
  reject(invalid);
  invalid = make_report();
  invalid.receiverClockEpoch = 0;
  reject(invalid);
  invalid = make_report();
  invalid.packetCount = TF_MAX_PACKETS + 1;
  reject(invalid);
  invalid = make_report();
  invalid.packetCount = 0;
  reject(invalid);
  invalid = make_report();
  invalid.baseExtendedSequence = UINT64_MAX;
  reject(invalid);
  invalid = make_report();
  invalid.status[0] = 4;
  reject(invalid);
  invalid = make_report();
  invalid.firstArrivalTimeUs[0] = 0;
  reject(invalid);
  invalid = make_report();
  invalid.firstArrivalTimeUs[1] = invalid.receiverSampleTimeUs + 1;
  reject(invalid);
  invalid = make_report();
  invalid.receiverSampleTimeUs = UINT64_MAX;
  reject(invalid);
  invalid = make_report();
  invalid.receiverSampleTimeUs = TF_MAX_ARRIVAL_AGE_US + 1ull;
  invalid.firstArrivalTimeUs[1] = 0;
  reject(invalid);
  const auto valid = make_report();
  EXPECT_EQ(TfEncodeReport(&valid, nullptr, output.size()), 0u);
  EXPECT_EQ(TfEncodeReport(&valid, output.data(), 1), 0u);
  EXPECT_EQ(output, unchanged);
}

TEST(TransportFeedbackWire, MaximumArrivalAgeAnd64BitClockAreExact) {
  auto r = make_report(2);
  r.receiverSampleTimeUs = INT64_MAX;
  r.firstArrivalTimeUs[1] = r.receiverSampleTimeUs - TF_MAX_ARRIVAL_AGE_US;
  r.receiverClockEpoch = UINT64_MAX;
  const auto bytes = encode(r);
  TF_PACKET_REPORT decoded {};
  ASSERT_TRUE(TfDecodeReport(bytes.data(), bytes.size(), &decoded));
  EXPECT_EQ(decoded.receiverSampleTimeUs, static_cast<uint64_t>(INT64_MAX));
  EXPECT_EQ(decoded.receiverClockEpoch, UINT64_MAX);
  EXPECT_EQ(decoded.firstArrivalTimeUs[1], r.firstArrivalTimeUs[1]);
  auto underflow = encode(make_report(2));
  std::fill(underflow.begin() + 32, underflow.begin() + 40, 0);
  EXPECT_FALSE(TfDecodeReport(underflow.data(), underflow.size(), &decoded));
}

TEST(TransportFeedbackWire, SenderReadyUsesSuccessfulWatermarkAndStrictLimits) {
  transport::wire_feedback_t state(42, true);
  ASSERT_TRUE(state.commit_success(sent(100)));
  ASSERT_TRUE(state.commit_success(sent(105)));
  const auto ready = state.ready(1000);
  ASSERT_TRUE(ready);
  EXPECT_EQ(ready->submittedThroughExclusive, 106u);
  std::array<uint8_t, TF_READY_BYTES> bytes {};
  ASSERT_EQ(TfEncodeReady(&*ready, bytes.data(), bytes.size()), bytes.size());
  TF_READY decoded {};
  ASSERT_TRUE(TfDecodeReady(bytes.data(), bytes.size(), &decoded));
  EXPECT_EQ(decoded.connectionEpoch, 42u);
  EXPECT_EQ(decoded.submittedThroughExclusive, 106u);
  EXPECT_EQ(decoded.maxFeedbackWireBytesPerSecond, 125000u);
  std::array<TF_READY, 5> invalids { *ready, *ready, *ready, *ready, *ready };
  invalids[0].maxPacketsPerReport = 0;
  invalids[1].reportIntervalMs = 19;
  invalids[2].maxFeedbackWireBytesPerSecond = 1048577;
  invalids[3].connectionEpoch = 0;
  invalids[4].senderSampleTimeUs = UINT64_MAX;
  for (const auto &invalid : invalids) {
    EXPECT_EQ(TfEncodeReady(&invalid, bytes.data(), bytes.size()), 0u);
  }
  for (size_t i = 0; i < bytes.size(); ++i) EXPECT_FALSE(TfDecodeReady(bytes.data(), i, &decoded));
  bytes[6] = 1;
  EXPECT_FALSE(TfDecodeReady(bytes.data(), bytes.size(), &decoded));
  transport::wire_feedback_t disabled(42, false);
  EXPECT_FALSE(disabled.ready(1000));
}

TEST(TransportFeedbackWire, Unwrap24BitUsesReferenceAndRejectsAmbiguousOrUnrepresentableRanges) {
  uint64_t sequence = 77;
  EXPECT_TRUE(TfUnwrapSequence24(3, 0xfffffe, &sequence));
  EXPECT_EQ(sequence, 0x1000003u);
  EXPECT_TRUE(TfUnwrapSequence24(0xfffffe, 0x1000003, &sequence));
  EXPECT_EQ(sequence, 0xfffffeu);
  EXPECT_TRUE(TfUnwrapSequence24(100, 100, &sequence));
  EXPECT_EQ(sequence, 100u);
  EXPECT_FALSE(TfUnwrapSequence24(0x800064, 100, &sequence));
  EXPECT_EQ(sequence, 100u);
  EXPECT_FALSE(TfUnwrapSequence24(0x1000000, 100, &sequence));
  EXPECT_FALSE(TfUnwrapSequence24(0xffffff, 0, &sequence));
  EXPECT_FALSE(TfUnwrapSequence24(0, UINT64_MAX, &sequence));
  for (uint64_t base = (1ull << 48) + 0xffff00; base < (1ull << 48) + 0x1000200; ++base) {
    ASSERT_TRUE(TfUnwrapSequence24(static_cast<uint32_t>(base + 1) & 0xffffff, base, &sequence));
    ASSERT_EQ(sequence, base + 1);
  }
}

TEST(TransportFeedbackWire, AuthenticatedDecodedInputIntersectsOnlyCommittedPackets) {
  transport::wire_feedback_t state(42, true);
  ASSERT_TRUE(state.commit_success(sent(101)));
  ASSERT_TRUE(state.commit_success(sent(102)));
  const auto bytes = encode(make_report());
  const auto result = state.apply_wire(bytes, 1000);
  ASSERT_EQ(result.result, transport::report_result_e::accepted);
  EXPECT_EQ(result.changes.size(), 2u);
  EXPECT_EQ(result.unmatched_packets, 2u);
  EXPECT_EQ(state.snapshot().ledger.received_packets, 1u);
  EXPECT_EQ(state.snapshot().ledger.missing_declarations, 1u);
  EXPECT_EQ(state.apply_wire(bytes, 1001).result, transport::report_result_e::duplicate);
  auto late = make_report();
  late.reportSequence = 2;
  late.status[2] = TF_RECEIVED;
  late.firstArrivalTimeUs[2] = 99999;
  EXPECT_EQ(state.apply_wire(encode(late), 1002).result, transport::report_result_e::accepted);
  EXPECT_EQ(state.snapshot().ledger.late_corrections, 1u);
  EXPECT_EQ(state.snapshot().ledger.committed_packets, 2u);
  late.reportSequence = 3;
  late.connectionEpoch = 43;
  EXPECT_EQ(state.apply_wire(encode(late), 1003).result, transport::report_result_e::wrong_epoch);
  EXPECT_EQ(state.snapshot().ledger.committed_packets, 2u);
}

TEST(TransportFeedbackWire, DisabledMalformedAndRateLimitedInputNeverChangesLoss) {
  transport::wire_feedback_t disabled(42, false);
  const auto report = encode(make_report());
  EXPECT_EQ(disabled.apply_wire(report, 1000).result, transport::report_result_e::invalid);
  transport::wire_feedback_t state(42, true);
  const std::vector<uint8_t> invalid(TF_MAX_REPORT_BYTES, 0);
  for (int i = 0; i < 1000; ++i) state.apply_wire(invalid, 1000);
  // Exhaust the final remainder with smaller malformed datagrams as well.
  for (int i = 0; i < 1000; ++i) state.apply_wire({}, 1000);
  auto status = state.snapshot();
  EXPECT_GT(status.rejected_reports, 0u);
  EXPECT_GT(status.rate_limited_reports, 0u);
  EXPECT_EQ(status.ledger.missing_declarations, 0u);
  EXPECT_EQ(state.apply_wire(report, 1000).result, transport::report_result_e::invalid);
  EXPECT_EQ(state.apply_wire(report, 1001000).result, transport::report_result_e::accepted);
  EXPECT_EQ(state.apply_wire(report, 999).result, transport::report_result_e::invalid);
}

TEST(TransportFeedbackWire, ConcurrentCommitFeedbackAndReadersKeepOneSessionLedger) {
  transport::wire_feedback_t state(42, true);
  std::thread sender([&] { for (uint64_t i = 0; i < 1000; ++i) EXPECT_TRUE(state.commit_success(sent(i))); });
  std::thread observer([&] {
    for (unsigned i = 0; i < 1000; ++i) {
      auto r = make_report(1);
      r.reportSequence = i + 1;
      r.baseExtendedSequence = i;
      r.status[0] = TF_RECEIVED;
      r.firstArrivalTimeUs[0] = 1000;
      state.apply_wire(encode(r), 1000000 + static_cast<int64_t>(i) * 50000);
      EXPECT_TRUE(state.snapshot().ledger.counters_valid);
    }
  });
  sender.join();
  observer.join();
  const auto status = state.snapshot();
  EXPECT_EQ(status.ledger.committed_packets, 1000u);
  EXPECT_LE(status.ledger.received_packets, 1000u);
  EXPECT_EQ(status.ledger.missing_declarations, 0u);
  EXPECT_EQ(status.submitted_through_exclusive, 1000u);
}

TEST(TransportFeedbackWire, OwnedEventsPreserveAuthoritativeMetadataAndApplyExactlyOnce) {
  transport::wire_feedback_t state(42, true);
  const auto first = state.commit_success_event(sent(101));
  ASSERT_TRUE(first);
  EXPECT_EQ(first->connection_epoch, 42u);
  EXPECT_EQ(first->event_sequence, 1u);
  EXPECT_EQ(first->commit_ordinal, 1u);
  EXPECT_EQ(first->sent.send_time_us, 101);
  EXPECT_EQ(first->data_in_flight_bytes, 1234u);
  ASSERT_TRUE(state.commit_success(sent(102)));  // Compatibility path, once.
  transport::feedback_event_t event;
  {
    auto bytes = encode(make_report());
    event = state.apply_wire_event(bytes, 1000);
    std::fill(bytes.begin(), bytes.end(), 0);  // The event cannot borrow this data.
  }
  EXPECT_EQ(event.connection_epoch, 42u);
  EXPECT_EQ(event.event_sequence, 3u);
  EXPECT_EQ(event.receiver_clock_epoch, 1u);
  EXPECT_EQ(event.receiver_sample_time_us, 100000);
  EXPECT_EQ(event.processing_time_us, 1000);
  EXPECT_EQ(event.data_in_flight_bytes, 0u);
  ASSERT_EQ(event.feedback.result, transport::report_result_e::accepted);
  ASSERT_EQ(event.feedback.changes.size(), 2u);
  EXPECT_EQ(event.feedback.changes.front().sent.extended_sequence, 101u);
  EXPECT_EQ(event.feedback.changes.front().sent.send_time_us, 101);
  EXPECT_EQ(event.feedback.changes.front().first_arrival_us, 99999);
  EXPECT_EQ(state.snapshot().accepted_reports, 1u);
  EXPECT_EQ(state.snapshot().ledger.received_packets, 1u);
  EXPECT_EQ(state.snapshot().ledger.missing_declarations, 1u);
  const auto duplicate = state.apply_wire_event(encode(make_report()), 1100);
  EXPECT_EQ(duplicate.feedback.result, transport::report_result_e::duplicate);
  EXPECT_EQ(duplicate.event_sequence, 0u);
  EXPECT_TRUE(duplicate.feedback.changes.empty());
  auto late = make_report();
  late.reportSequence = 2;
  late.status[2] = TF_RECEIVED;
  late.firstArrivalTimeUs[2] = 99998;
  EXPECT_EQ(state.apply_wire(encode(late), 1200).changes.size(), 1u);
  EXPECT_EQ(state.snapshot().ledger.late_corrections, 1u);
  EXPECT_EQ(state.snapshot().accepted_reports, 2u);
}

TEST(TransportFeedbackWire, EventSequenceExhaustionRejectsBeforeLedgerMutation) {
  transport::wire_feedback_t send_exhaustion(42, true, 10, UINT64_MAX - 1);
  const auto last = send_exhaustion.commit_success_event(sent(101));
  ASSERT_TRUE(last);
  EXPECT_EQ(last->event_sequence, UINT64_MAX);
  EXPECT_FALSE(send_exhaustion.commit_success_event(sent(102)));
  const auto rejected = send_exhaustion.apply_wire_event(encode(make_report()), 1000);
  EXPECT_EQ(rejected.event_sequence, 0u);
  EXPECT_EQ(rejected.feedback.result, transport::report_result_e::invalid);
  EXPECT_EQ(send_exhaustion.snapshot().ledger.committed_packets, 1u);
  EXPECT_EQ(send_exhaustion.snapshot().ledger.received_packets, 0u);
  EXPECT_EQ(send_exhaustion.snapshot().ledger.missing_declarations, 0u);

  transport::wire_feedback_t feedback_exhaustion(42, true, 10, UINT64_MAX - 2);
  ASSERT_TRUE(feedback_exhaustion.commit_success(sent(101)));
  const auto final_report = feedback_exhaustion.apply_wire_event(encode(make_report()), 1000);
  EXPECT_EQ(final_report.event_sequence, UINT64_MAX);
  EXPECT_EQ(final_report.feedback.result, transport::report_result_e::accepted);
  auto next = make_report();
  next.reportSequence = 2;
  EXPECT_EQ(feedback_exhaustion.apply_wire_event(encode(next), 1100).feedback.result, transport::report_result_e::invalid);
  EXPECT_EQ(feedback_exhaustion.snapshot().accepted_reports, 1u);
  EXPECT_EQ(feedback_exhaustion.snapshot().ledger.received_packets, 1u);
}
