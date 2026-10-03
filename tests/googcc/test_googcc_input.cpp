#include "src/transport/googcc_adapter.h"
#include "src/transport/transport_feedback_wire.h"

#include <array>
#include <deque>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

namespace {
  using namespace transport;

  googcc_config_t
  configuration() {
    googcc_config_t result;
    result.connection_epoch = 7;
    return result;
  }

  sent_packet_t
  packet(std::uint64_t sequence, std::int64_t time_us) {
    return { sequence, time_us, 1280, 37, 12, packet_kind_e::data, {} };
  }

  packet_report_t
  report(std::uint64_t report_sequence, std::int64_t time_us,
    std::span<const packet_observation_t> observations,
    std::int64_t receiver_sample_us, std::uint64_t receiver_epoch = 1) {
    return { 7, report_sequence, receiver_epoch, time_us, observations, receiver_sample_us };
  }

  std::vector<std::uint8_t>
  encode(const packet_report_t &message) {
    TF_PACKET_REPORT wire {};
    wire.connectionEpoch = message.connection_epoch;
    wire.reportSequence = message.report_sequence;
    wire.receiverClockEpoch = message.receiver_clock_epoch;
    wire.receiverSampleTimeUs = static_cast<std::uint64_t>(message.receiver_sample_time_us);
    if (message.packets.empty()) return {};
    wire.baseExtendedSequence = message.packets.front().extended_sequence;
    const auto count = message.packets.back().extended_sequence - wire.baseExtendedSequence + 1;
    if (count == 0 || count > TF_MAX_PACKETS) {
      ADD_FAILURE() << "Replay report exceeds bounded wire coverage: " << count;
      return {};
    }
    wire.packetCount = static_cast<std::uint16_t>(count);
    for (std::size_t i = 0; i < count; ++i) {
      wire.status[i] = TF_UNKNOWN;
      wire.firstArrivalTimeUs[i] = TF_NO_ARRIVAL;
    }
    for (const auto &observation : message.packets) {
      const auto index = observation.extended_sequence - wire.baseExtendedSequence;
      wire.status[index] = static_cast<std::uint8_t>(observation.status);
      wire.firstArrivalTimeUs[index] = observation.status == packet_status_e::received ?
                                         static_cast<std::uint64_t>(observation.first_arrival_us) :
                                         TF_NO_ARRIVAL;
    }
    std::vector<std::uint8_t> bytes(TF_MAX_REPORT_BYTES);
    bytes.resize(TfEncodeReport(&wire, bytes.data(), bytes.size()));
    return bytes;
  }

  // This is the production wire decoder and the single authoritative ledger,
  // outside the adapter. The fixture consumes each owned event on one owner.
  struct session_t {
    explicit session_t(const googcc_config_t &config, std::size_t authority_capacity = 16384):
        wire(config.connection_epoch, true, authority_capacity), controller(config) {}

    bool
    on_successful_send(const sent_packet_t &sent) {
      const auto event = wire.commit_success_event(sent);
      return event && controller.on_successful_send(*event);
    }
    feedback_result_t
    on_feedback(const packet_report_t &message) {
      const auto event = wire.apply_wire_event(encode(message), message.received_at_us);
      if (event.feedback.result == report_result_e::accepted)
        EXPECT_TRUE(controller.on_feedback(event));
      else
        EXPECT_FALSE(controller.on_feedback(event));
      return event.feedback;
    }
    bool
    process_interval(std::int64_t time_us) { return controller.process_interval(time_us); }
    std::vector<googcc_probe_t>
    take_probe_requests() { return controller.take_probe_requests(); }
    const googcc_snapshot_t &
    snapshot() const { return controller.snapshot(); }
    send_ledger_snapshot_t
    ledger_snapshot() const { return wire.snapshot().ledger; }

    wire_feedback_t wire;
    googcc_adapter_t controller;
  };

  TEST(GoogCcInput, PinnedControllerInitializesAndRequestsRealProbeClusters) {
    session_t adapter(configuration());
    EXPECT_EQ(adapter.snapshot().target_kbps, 30000);
    EXPECT_GE(adapter.snapshot().pacing_kbps, adapter.snapshot().target_kbps);
    const auto probes = adapter.take_probe_requests();
    ASSERT_EQ(probes.size(), 2);
    EXPECT_GT(probes.front().target_kbps, 30000);
    EXPECT_GT(probes.front().duration_us, 0);
    EXPECT_GT(probes.front().minimum_packets, 0);
    EXPECT_TRUE(adapter.take_probe_requests().empty());
  }

  TEST(GoogCcInput, QueuePushbackReducesProductionWithoutShrinkingTheNetworkDrainRate) {
    auto config = configuration();
    config.pacer_queue_feedback = config.queue_pushback = true;
    session_t empty(config), busy(config);
    for (auto *adapter : { &empty, &busy }) {
      ASSERT_TRUE(adapter->on_successful_send(packet(0, 0)));
      const std::array received { packet_observation_t { 0, packet_status_e::received, 9000000 } };
      ASSERT_EQ(adapter->on_feedback(report(1, 1000, received, 9001000)).result, report_result_e::accepted);
    }
    ASSERT_TRUE(empty.controller.process_interval(1000, googcc_queue_sample_t { 7, 1000, 0, true, false }));
    ASSERT_TRUE(busy.controller.process_interval(1000, googcc_queue_sample_t { 7, 1000, 2000000, true, false }));
    EXPECT_GT(busy.snapshot().encoder_reduce_ratio, empty.snapshot().encoder_reduce_ratio);
    EXPECT_EQ(busy.snapshot().target_kbps, empty.snapshot().target_kbps);
    EXPECT_EQ(busy.snapshot().pacing_kbps, empty.snapshot().pacing_kbps);
    EXPECT_EQ(busy.snapshot().pacer_queue_ip_bytes, 2000000U);
    EXPECT_EQ(busy.ledger_snapshot().committed_packets, 1U);
    EXPECT_EQ(busy.ledger_snapshot().data_in_flight_bytes, 0U);
    ASSERT_TRUE(busy.controller.process_interval(26000, googcc_queue_sample_t { 7, 26000, 0, true, false }));
    EXPECT_EQ(busy.snapshot().encoder_reduce_ratio, 0);
    EXPECT_EQ(busy.snapshot().pacer_queue_ip_bytes, 0U);
  }

  TEST(GoogCcInput, QueueFeedbackRejectsUnknownForeignStaleStoppedAndOverflowWithoutAdvancingTime) {
    auto config = configuration();
    config.pacer_queue_feedback = true;
    session_t adapter(config);
    EXPECT_FALSE(adapter.controller.process_interval(1000));
    for (const auto queue : { googcc_queue_sample_t { 8, 1000, 0, true, false },
           googcc_queue_sample_t { 7, 999, 0, true, false }, googcc_queue_sample_t { 7, 1000, 0, false, false },
           googcc_queue_sample_t { 7, 1000, 0, true, true },
           googcc_queue_sample_t { 7, 1000, UINT64_MAX, true, false } }) {
      EXPECT_FALSE(adapter.controller.process_interval(1000, queue));
    }
    EXPECT_EQ(adapter.snapshot().rejected_queue_samples, 6U);
    EXPECT_EQ(adapter.snapshot().accepted_queue_samples, 0U);
    EXPECT_FALSE(adapter.snapshot().pacer_queue_ip_bytes);
    ASSERT_TRUE(adapter.on_successful_send(packet(0, 500)));
    ASSERT_TRUE(adapter.controller.process_interval(500, googcc_queue_sample_t { 7, 500, 1280, true, false }));
    EXPECT_EQ(adapter.snapshot().accepted_queue_samples, 1U);
    config.pacer_queue_feedback = false;
    config.queue_pushback = true;
    EXPECT_THROW(googcc_adapter_t { config }, std::invalid_argument);
  }

  TEST(GoogCcInput, ActualLowSendRateEntersAlrAndResumedTrafficLeavesWithoutFalseLoss) {
    auto config = configuration();
    config.maximum_kbps = config.initial_kbps;
    session_t adapter(config);
    adapter.take_probe_requests();
    std::uint64_t sequence = 0, reports = 0;
    std::vector<packet_observation_t> pending;
    std::ofstream trace("googcc-alr-trace.csv");
    ASSERT_TRUE(trace);
    trace << "time_us,sends,target_kbps,application_limited,target_updated_at_us\n";
    const auto deliver = [&](std::int64_t at) {
      const auto id = sequence++;
      auto sent = packet(id, at);
      sent.kind = id % 6 == 5 ? packet_kind_e::fec : packet_kind_e::data;
      ASSERT_TRUE(adapter.on_successful_send(sent));
      pending.push_back({ id, packet_status_e::received, at + 9000000000 });
    };
    const auto flush = [&](std::int64_t at) {
      ASSERT_EQ(adapter.on_feedback(report(++reports, at, pending, at + 9000000000)).result, report_result_e::accepted);
      pending.clear();
    };
    // 512 kbps actual IP traffic, far below the 30 Mbps available target.
    for (std::int64_t at = 0; at <= 3000000; at += 20000) {
      deliver(at);
      flush(at);
      ASSERT_TRUE(adapter.process_interval(at));
      trace << at << ',' << sequence << ',' << adapter.snapshot().target_kbps << ','
            << adapter.snapshot().application_limited << ',' << adapter.snapshot().target_updated_at_us << '\n';
    }
    EXPECT_TRUE(adapter.snapshot().application_limited);
    EXPECT_GE(adapter.snapshot().target_kbps, 29000);
    const auto low_updated = adapter.snapshot().target_updated_at_us;
    // Resume 30.72 Mbps of genuine successful submissions, including FEC.
    // Pinned screenshare defaults use 80% utilization and -60% exit debt:
    // drain that upstream hysteresis rather than force an immediate exit.
    for (std::int64_t at = 3001000; at <= 6500000; at += 1000) {
      for (int i = 0; i < 3; ++i) deliver(at);
      if (at % 20000 == 0) flush(at);
      ASSERT_TRUE(adapter.process_interval(at));
      if (at % 20000 == 0) {
        trace << at << ',' << sequence << ',' << adapter.snapshot().target_kbps << ','
              << adapter.snapshot().application_limited << ',' << adapter.snapshot().target_updated_at_us << '\n';
      }
    }
    EXPECT_FALSE(adapter.snapshot().application_limited);
    EXPECT_GT(adapter.snapshot().target_updated_at_us, low_updated);
    EXPECT_EQ(adapter.snapshot().accepted_sends, sequence);
    EXPECT_EQ(adapter.ledger_snapshot().received_packets, sequence);
    EXPECT_EQ(adapter.ledger_snapshot().missing_declarations, 0U);
    EXPECT_EQ(adapter.ledger_snapshot().data_in_flight_bytes, 0U);
    EXPECT_EQ(adapter.snapshot().rejected_send_events, 0U);
    EXPECT_EQ(adapter.snapshot().rejected_feedback_events, 0U);
  }

  TEST(GoogCcInput, TimersAndRejectedSendsCannotInventAlrOrSuccessfulTraffic) {
    session_t adapter(configuration());
    adapter.take_probe_requests();
    ASSERT_TRUE(adapter.on_successful_send(packet(0, 0)));
    ASSERT_TRUE(adapter.process_interval(10000000));
    EXPECT_FALSE(adapter.snapshot().application_limited);
    const auto updated = adapter.snapshot().target_updated_at_us;
    auto duplicate = successful_send_event_t {};
    duplicate.connection_epoch = 7;
    duplicate.event_sequence = 2;
    duplicate.sent = packet(0, 10000000);
    EXPECT_FALSE(adapter.controller.on_successful_send(duplicate));
    EXPECT_EQ(adapter.snapshot().target_updated_at_us, updated);
    EXPECT_EQ(adapter.snapshot().accepted_sends, 1U);
    EXPECT_FALSE(adapter.snapshot().application_limited);
    ASSERT_TRUE(adapter.on_successful_send(packet(1, 10000000)));
    ASSERT_TRUE(adapter.process_interval(10000000));
    EXPECT_TRUE(adapter.snapshot().application_limited);
    EXPECT_EQ(adapter.snapshot().accepted_sends, 2U);
  }

  TEST(GoogCcInput, LowRateAlrDoesNotHideRealLossOrRaiseTheCeiling) {
    auto config = configuration();
    config.maximum_kbps = config.initial_kbps;
    session_t adapter(config);
    adapter.take_probe_requests();
    std::uint64_t sequence = 0, reports = 0, missing = 0;
    for (std::int64_t at = 0; at <= 15000000; at += 20000) {
      const auto id = sequence++;
      ASSERT_TRUE(adapter.on_successful_send(packet(id, at)));
      const bool lost = at > 3000000 && id % 4 == 0;
      missing += lost;
      const std::array observations { packet_observation_t { id,
        lost ? packet_status_e::missing : packet_status_e::received,
        lost ? -1 : at + 9000000000 } };
      ASSERT_EQ(adapter.on_feedback(report(++reports, at, observations, at + 9000000000)).result, report_result_e::accepted);
      ASSERT_TRUE(adapter.process_interval(at));
      EXPECT_LE(adapter.snapshot().target_kbps, config.maximum_kbps);
      if (at == 3000000) {
        EXPECT_TRUE(adapter.snapshot().application_limited);
      }
    }
    EXPECT_GT(missing, 0U);
    EXPECT_LT(adapter.snapshot().target_kbps, 29000);
    EXPECT_EQ(adapter.snapshot().feedback_packet_changes, sequence);
    EXPECT_EQ(adapter.ledger_snapshot().missing_declarations, missing);
    EXPECT_EQ(adapter.ledger_snapshot().received_packets + missing, sequence);
    EXPECT_EQ(adapter.ledger_snapshot().data_in_flight_bytes, 0U);
    EXPECT_EQ(adapter.snapshot().rejected_send_events, 0U);
    EXPECT_EQ(adapter.snapshot().rejected_feedback_events, 0U);
  }

  TEST(GoogCcInput, OnlyNewMatchedTransitionsReachTheController) {
    session_t adapter(configuration());
    ASSERT_TRUE(adapter.on_successful_send(packet(10, 0)));
    ASSERT_TRUE(adapter.on_successful_send(packet(12, 500)));
    const std::array observations {
      packet_observation_t { 10, packet_status_e::missing, -1 },
      packet_observation_t { 11, packet_status_e::missing, -1 },
      packet_observation_t { 12, packet_status_e::received, 9000500 }
    };
    const auto message = report(1, 1000, observations, 9001000);
    const auto result = adapter.on_feedback(message);
    ASSERT_EQ(result.result, report_result_e::accepted);
    EXPECT_EQ(result.unmatched_packets, 1);
    EXPECT_EQ(adapter.snapshot().feedback_packet_changes, 2);
    EXPECT_EQ(adapter.ledger_snapshot().missing_declarations, 1);
    EXPECT_EQ(adapter.on_feedback(message).result, report_result_e::duplicate);
    EXPECT_EQ(adapter.on_feedback(report(2, 1000, observations, 9001000)).result, report_result_e::accepted);
    EXPECT_EQ(adapter.snapshot().feedback_batches, 1);
    const std::array late { packet_observation_t { 10, packet_status_e::received, 9000900 } };
    EXPECT_EQ(adapter.on_feedback(report(3, 1200, late, 9001200)).changes.size(), 1);
    EXPECT_TRUE(adapter.on_feedback(report(4, 1400, late, 9001400)).changes.empty());
    EXPECT_EQ(adapter.snapshot().feedback_packet_changes, 3);
    EXPECT_EQ(adapter.ledger_snapshot().received_packets, 2);
    EXPECT_EQ(adapter.ledger_snapshot().received_ip_bytes, 2560);
    EXPECT_EQ(adapter.ledger_snapshot().late_corrections, 1);
  }

  TEST(GoogCcInput, CoverageWatermarkUsesMappedActualSendTimeAndDoesNotRegressOnLateChanges) {
    auto config = configuration();
    config.history_capacity = 2;
    session_t adapter(config);
    ASSERT_TRUE(adapter.on_successful_send(packet(1, 1000)));
    ASSERT_TRUE(adapter.on_successful_send(packet(2, 2000)));
    ASSERT_TRUE(adapter.on_successful_send(packet(3, 3000)));
    const std::array newer { packet_observation_t { 1, packet_status_e::received, 9010000 },
      packet_observation_t { 3, packet_status_e::received, 9012000 } };
    ASSERT_EQ(adapter.on_feedback(report(1, 20000, newer, 9020000)).result, report_result_e::accepted);
    EXPECT_EQ(adapter.snapshot().unmapped_feedback_changes, 1U);
    EXPECT_EQ(adapter.snapshot().last_covered_send_us, 3000);
    const std::array older { packet_observation_t { 2, packet_status_e::received, 9011000 } };
    ASSERT_EQ(adapter.on_feedback(report(2, 1200000, older, 9020000)).result, report_result_e::accepted);
    EXPECT_EQ(adapter.snapshot().last_covered_feedback_us, 1200000);
    EXPECT_EQ(adapter.snapshot().last_covered_send_us, 3000);
    EXPECT_EQ(adapter.snapshot().feedback_packet_changes, 2U);
    EXPECT_EQ(adapter.ledger_snapshot().received_packets, 3U);
  }

  TEST(GoogCcInput, EmptyReceiverClockResetInvalidatesCoverageUntilNewMappedChanges) {
    session_t adapter(configuration());
    ASSERT_TRUE(adapter.on_successful_send(packet(1, 1000)));
    const std::array initial { packet_observation_t { 1, packet_status_e::received, 9010000 } };
    ASSERT_EQ(adapter.on_feedback(report(1, 20000, initial, 9020000)).result, report_result_e::accepted);
    EXPECT_EQ(adapter.snapshot().last_covered_send_us, 1000);
    const std::array unknown { packet_observation_t { 2, packet_status_e::unknown, -1 } };
    ASSERT_EQ(adapter.on_feedback(report(2, 25000, unknown, 3000000000, 2)).result, report_result_e::accepted);
    EXPECT_EQ(adapter.snapshot().last_accepted_feedback_us, 25000);
    EXPECT_EQ(adapter.snapshot().last_covered_feedback_us, -1);
    EXPECT_EQ(adapter.snapshot().last_covered_send_us, -1);
    EXPECT_EQ(adapter.snapshot().receiver_clock_resets, 1U);
    EXPECT_EQ(adapter.ledger_snapshot().received_packets, 1U);
    ASSERT_TRUE(adapter.on_successful_send(packet(2, 30000)));
    const std::array current { packet_observation_t { 2, packet_status_e::received, 3000020000 } };
    ASSERT_EQ(adapter.on_feedback(report(3, 50000, current, 3000030000, 2)).result, report_result_e::accepted);
    EXPECT_EQ(adapter.snapshot().last_covered_send_us, 30000);
    EXPECT_EQ(adapter.snapshot().last_covered_feedback_us, 50000);
    EXPECT_EQ(adapter.ledger_snapshot().received_packets, 2U);
  }

  TEST(GoogCcInput, ReceiverClockChangeResetsEstimatorWithoutDuplicatingLedgerBytes) {
    session_t adapter(configuration());
    ASSERT_TRUE(adapter.on_successful_send(packet(1, 0)));
    const std::array initial { packet_observation_t { 1, packet_status_e::received, 9000050 } };
    EXPECT_EQ(adapter.on_feedback(report(1, 100, initial, 9000100)).result, report_result_e::accepted);
    const std::array new_clock { packet_observation_t { 1, packet_status_e::received, 5 } };
    const auto reset = adapter.on_feedback(report(2, 1000, new_clock, 10, 2));
    EXPECT_EQ(reset.result, report_result_e::accepted);
    EXPECT_TRUE(reset.receiver_clock_changed);
    EXPECT_EQ(adapter.snapshot().receiver_clock_resets, 1);
    EXPECT_GE(adapter.snapshot().discarded_probe_requests, 2u);
    EXPECT_EQ(adapter.snapshot().feedback_packet_changes, 1);
    EXPECT_EQ(adapter.ledger_snapshot().received_ip_bytes, 1280);
    EXPECT_EQ(adapter.on_feedback(report(3, 1100, initial, 9001100, 1)).result, report_result_e::wrong_epoch);
  }

  TEST(GoogCcInput, InvalidInputsNeverConsumeAReportOrInventASentPacket) {
    auto invalid_config = configuration();
    invalid_config.minimum_kbps = 0;
    EXPECT_THROW(googcc_adapter_t { invalid_config }, std::invalid_argument);
    session_t adapter(configuration());
    EXPECT_FALSE(adapter.on_successful_send(packet(std::numeric_limits<std::uint64_t>::max(), 0)));
    auto invalid = packet(1, 0);
    invalid.probe.cluster_id = 2;
    EXPECT_FALSE(adapter.on_successful_send(invalid));
    ASSERT_TRUE(adapter.on_successful_send(packet(1, 100)));
    EXPECT_FALSE(adapter.on_successful_send(packet(2, 99)));
    const std::array observations { packet_observation_t { 1, packet_status_e::received, 9000000 } };
    EXPECT_EQ(adapter.on_feedback(report(1, 99, observations, 9000100)).result, report_result_e::invalid);
    EXPECT_EQ(adapter.on_feedback(report(1, 1000, observations, -1)).result, report_result_e::invalid);
    EXPECT_EQ(adapter.on_feedback(report(1, 1000, observations, 8999999)).result, report_result_e::invalid);
    EXPECT_EQ(adapter.on_feedback(report(1, 1000, observations, 9000100)).result, report_result_e::accepted);
    EXPECT_EQ(adapter.ledger_snapshot().committed_packets, 1);
    EXPECT_FALSE(adapter.process_interval(999));
    EXPECT_TRUE(adapter.process_interval(1025));
  }

  TEST(GoogCcInput, FixedReceiverClockOffsetDoesNotChangeEstimates) {
    session_t first(configuration()), second(configuration());
    constexpr std::int64_t first_offset = 1000000, second_offset = 9000000000;
    for (std::int64_t batch = 0; batch < 100; ++batch) {
      std::vector<packet_observation_t> first_observations, second_observations;
      for (std::int64_t index = 0; index < 50; ++index) {
        const auto sequence = static_cast<std::uint64_t>(batch * 50 + index);
        const auto time_us = batch * 50000 + index * 500;
        ASSERT_TRUE(first.on_successful_send(packet(sequence, time_us)));
        ASSERT_TRUE(second.on_successful_send(packet(sequence, time_us)));
        const auto arrival_us = time_us + 10000 + (batch > 20 && batch < 50 ? batch * 200 : 0);
        first_observations.push_back({ sequence, packet_status_e::received, arrival_us + first_offset });
        second_observations.push_back({ sequence, packet_status_e::received, arrival_us + second_offset });
      }
      const auto feedback_time = (batch + 1) * 50000;
      ASSERT_EQ(first.on_feedback(report(batch + 1, feedback_time, first_observations,
                                    feedback_time + first_offset))
                  .result,
        report_result_e::accepted);
      ASSERT_EQ(second.on_feedback(report(batch + 1, feedback_time, second_observations,
                                     feedback_time + second_offset))
                  .result,
        report_result_e::accepted);
      ASSERT_TRUE(first.process_interval(feedback_time));
      ASSERT_TRUE(second.process_interval(feedback_time));
      EXPECT_EQ(first.snapshot().target_kbps, second.snapshot().target_kbps);
      EXPECT_EQ(first.snapshot().pacing_kbps, second.snapshot().pacing_kbps);
      EXPECT_EQ(first.snapshot().feedback_packet_changes, second.snapshot().feedback_packet_changes);
    }
  }

  TEST(GoogCcInput, ProbeMetadataReachesThePinnedProbeEstimator) {
    auto config = configuration();
    config.maximum_kbps = 100000;
    session_t adapter(config);
    const auto clusters = adapter.take_probe_requests();
    ASSERT_FALSE(clusters.empty());
    const auto &cluster = clusters.front();
    ASSERT_GT(cluster.target_kbps, config.initial_kbps);
    const auto spacing_us = 1280 * 8 * 1000 / cluster.target_kbps;
    std::vector<packet_observation_t> observations;
    for (int index = 0; index < cluster.minimum_packets + 3; ++index) {
      auto sent = packet(index + 1, 1000 + index * spacing_us);
      sent.kind = packet_kind_e::probe;
      sent.probe = { cluster.cluster_id, cluster.minimum_packets, cluster.minimum_packets * 1280,
        static_cast<std::int32_t>(cluster.target_kbps) };
      ASSERT_TRUE(adapter.on_successful_send(sent));
      observations.push_back({ sent.extended_sequence, packet_status_e::received, 9000000 + sent.send_time_us + 20000 });
    }
    const auto result = adapter.on_feedback(report(1, 30000, observations, 9025000));
    ASSERT_EQ(result.result, report_result_e::accepted);
    ASSERT_FALSE(result.changes.empty());
    EXPECT_EQ(result.changes.front().sent.probe.cluster_id, cluster.cluster_id);
    ASSERT_TRUE(adapter.process_interval(30000));
    EXPECT_GT(adapter.snapshot().target_kbps, config.initial_kbps);
    EXPECT_LE(adapter.snapshot().target_kbps, config.maximum_kbps);
    EXPECT_GT(adapter.snapshot().native_probe_successes, 0U);
    EXPECT_EQ(adapter.snapshot().last_native_probe_cluster, cluster.cluster_id);
    EXPECT_GT(adapter.snapshot().last_native_probe_bps, config.initial_kbps * 1000);
    EXPECT_EQ(adapter.snapshot().last_native_probe_at_us, 30000);
    EXPECT_EQ(adapter.snapshot().last_native_probe_failure, -1);
    const auto successes = adapter.snapshot().native_probe_successes;
    const std::array repeated { observations.front() };
    ASSERT_EQ(adapter.on_feedback(report(2, 31000, repeated, 9025000, 2)).result, report_result_e::accepted);
    EXPECT_EQ(adapter.snapshot().native_probe_successes, successes);
    EXPECT_EQ(adapter.snapshot().last_native_probe_cluster, -1);
    EXPECT_EQ(adapter.snapshot().last_native_probe_at_us, -1);
    EXPECT_EQ(adapter.snapshot().last_native_probe_bps, 0);
  }

  TEST(GoogCcInput, NativeProbeFailureIsObservedWithoutInventingAValidEstimate) {
    session_t adapter(configuration());
    const auto cluster = adapter.take_probe_requests().front();
    std::vector<packet_observation_t> observations;
    for (int index = 0; index < 8; ++index) {
      auto sent = packet(index + 1, 1000);  // Invalid zero sending interval.
      sent.probe = { cluster.cluster_id, 5, 6400, static_cast<std::int32_t>(cluster.target_kbps) };
      ASSERT_TRUE(adapter.on_successful_send(sent));
      observations.push_back({ sent.extended_sequence, packet_status_e::received, 9021000 + index * 300 });
    }
    ASSERT_EQ(adapter.on_feedback(report(1, 30000, observations, 9025000)).result, report_result_e::accepted);
    EXPECT_EQ(adapter.snapshot().native_probe_successes, 0U);
    EXPECT_GT(adapter.snapshot().native_probe_failures, 0U);
    EXPECT_EQ(adapter.snapshot().last_native_probe_cluster, cluster.cluster_id);
    EXPECT_EQ(adapter.snapshot().last_native_probe_failure, 0);  // Pinned invalid interval reason.
    EXPECT_EQ(adapter.snapshot().last_native_probe_bps, 0);
    EXPECT_EQ(adapter.snapshot().last_native_probe_at_us, 30000);
  }

  TEST(GoogCcInput, TranslatedProbeIdentityReachesNativeEstimatorAndReturnsPhysicalResult) {
    auto config = configuration();
    config.maximum_kbps = 100000;
    googcc_adapter_t baseline(config);
    const auto native_clusters = baseline.take_probe_requests();
    ASSERT_EQ(native_clusters.size(), 2U);
    config.first_probe_cluster_id = 37;
    session_t adapter(config);
    const auto clusters = adapter.take_probe_requests();
    ASSERT_EQ(clusters.size(), 2U);
    EXPECT_EQ(clusters.front().cluster_id, native_clusters.front().cluster_id + 37);
    EXPECT_EQ(adapter.snapshot().last_generated_probe_cluster, baseline.snapshot().last_generated_probe_cluster + 37);
    const auto cluster = clusters.front();
    std::vector<packet_observation_t> observations;
    for (int index = 0; index < 8; ++index) {
      auto sent = packet(index, 1000 + index * (1280 * 8 * 1000 / cluster.target_kbps));
      sent.probe = { cluster.cluster_id, cluster.minimum_packets, cluster.minimum_packets * 1280,
        static_cast<std::int32_t>(cluster.target_kbps) };
      ASSERT_TRUE(adapter.on_successful_send(sent));
      observations.push_back({ sent.extended_sequence, packet_status_e::received, 9000000 + sent.send_time_us + 20000 });
    }
    const auto feedback = adapter.on_feedback(report(1, 30000, observations, 9025000));
    ASSERT_EQ(feedback.result, report_result_e::accepted);
    EXPECT_EQ(feedback.changes.front().sent.probe.cluster_id, cluster.cluster_id);
    EXPECT_GT(adapter.snapshot().native_probe_successes, 0U);
    EXPECT_EQ(adapter.snapshot().last_native_probe_cluster, cluster.cluster_id);
    EXPECT_GT(adapter.snapshot().last_native_probe_bps, config.initial_kbps * 1000);
  }

  TEST(GoogCcInput, RecreatedControllerCannotMapDelayedOldClusterIntoItsNewNamespace) {
    auto config = configuration();
    wire_feedback_t wire { 7, true };
    auto previous = std::make_unique<googcc_adapter_t>(config);
    const auto old_cluster = previous->take_probe_requests().front();
    std::vector<packet_observation_t> old_observations;
    for (int index = 0; index < 8; ++index) {
      auto sent = packet(index, 1000 + index * 500);
      sent.probe = { old_cluster.cluster_id, 5, 6400, static_cast<std::int32_t>(old_cluster.target_kbps) };
      const auto event = wire.commit_success_event(sent);
      ASSERT_TRUE(event);
      ASSERT_TRUE(previous->on_successful_send(*event));
      old_observations.push_back({ sent.extended_sequence, packet_status_e::received, 9000000 + sent.send_time_us + 5000 });
    }
    config.first_probe_cluster_id = static_cast<std::int32_t>(previous->snapshot().last_generated_probe_cluster + 1);
    config.start_time_us = 10000;
    previous.reset();
    googcc_adapter_t current(config);
    const auto clusters = current.take_probe_requests();
    ASSERT_FALSE(clusters.empty());
    EXPECT_GT(clusters.front().cluster_id, old_cluster.cluster_id);
    auto bytes = encode(report(1, 20000, old_observations, 9020000));
    const auto old_feedback = wire.apply_wire_event(bytes, 20000);
    ASSERT_EQ(old_feedback.feedback.result, report_result_e::accepted);
    ASSERT_TRUE(current.on_feedback(old_feedback));
    EXPECT_EQ(current.snapshot().unmapped_feedback_changes, 8U);
    EXPECT_EQ(current.snapshot().last_covered_send_us, -1);
    EXPECT_EQ(current.snapshot().native_probe_successes, 0U);
    EXPECT_EQ(wire.snapshot().ledger.received_packets, 8U);
    EXPECT_EQ(old_feedback.feedback.changes.front().sent.probe.cluster_id, old_cluster.cluster_id);
    const auto cluster = clusters.front();
    std::vector<packet_observation_t> observations;
    for (int index = 0; index < 8; ++index) {
      auto sent = packet(index + 8, 21000 + index * 500);
      sent.probe = { cluster.cluster_id, 5, 6400, static_cast<std::int32_t>(cluster.target_kbps) };
      const auto event = wire.commit_success_event(sent);
      ASSERT_TRUE(event);
      ASSERT_TRUE(current.on_successful_send(*event));
      observations.push_back({ sent.extended_sequence, packet_status_e::received, 9000000 + sent.send_time_us + 5000 });
    }
    bytes = encode(report(2, 40000, observations, 9040000));
    const auto feedback = wire.apply_wire_event(bytes, 40000);
    ASSERT_TRUE(current.on_feedback(feedback));
    EXPECT_GT(current.snapshot().native_probe_successes, 0U);
    EXPECT_EQ(current.snapshot().last_native_probe_cluster, cluster.cluster_id);
    EXPECT_EQ(wire.snapshot().ledger.received_packets, 16U);
  }

  TEST(GoogCcInput, InvalidProbeNamespaceFailsBeforeUpstreamConstruction) {
    auto config = configuration();
    for (const std::int32_t invalid : { -1, std::numeric_limits<std::int32_t>::max() - 31 }) {
      config.first_probe_cluster_id = invalid;
      EXPECT_THROW(googcc_adapter_t adapter(config), std::invalid_argument);
    }
  }

  TEST(GoogCcInput, InsufficientProbeFeedbackDoesNotClaimEitherNativeResult) {
    session_t adapter(configuration());
    const auto cluster = adapter.take_probe_requests().front();
    std::vector<packet_observation_t> observations;
    for (int index = 0; index < 3; ++index) {
      auto sent = packet(index + 1, 1000 + index * 1000);
      sent.probe = { cluster.cluster_id, 5, 10000, static_cast<std::int32_t>(cluster.target_kbps) };
      ASSERT_TRUE(adapter.on_successful_send(sent));
      observations.push_back({ sent.extended_sequence, packet_status_e::received, 9020000 + sent.send_time_us });
    }
    ASSERT_EQ(adapter.on_feedback(report(1, 30000, observations, 9025000)).result, report_result_e::accepted);
    EXPECT_EQ(adapter.snapshot().native_probe_successes, 0U);
    EXPECT_EQ(adapter.snapshot().native_probe_failures, 0U);
    EXPECT_EQ(adapter.snapshot().last_native_probe_cluster, -1);
    EXPECT_EQ(adapter.snapshot().last_native_probe_at_us, -1);
  }

  TEST(GoogCcInput, DelayedOldClusterFeedbackCannotEstimateAfterReceiverClockReset) {
    session_t adapter(configuration());
    const auto old_cluster = adapter.take_probe_requests().front();
    ASSERT_TRUE(adapter.on_successful_send(packet(0, 0)));
    const std::array first { packet_observation_t { 0, packet_status_e::received, 9000000 } };
    ASSERT_EQ(adapter.on_feedback(report(1, 1000, first, 9001000)).result, report_result_e::accepted);
    std::vector<packet_observation_t> old_observations;
    for (int index = 0; index < 8; ++index) {
      auto sent = packet(index + 1, 2000 + index * 500);
      sent.probe = { old_cluster.cluster_id, 5, 6400, static_cast<std::int32_t>(old_cluster.target_kbps) };
      ASSERT_TRUE(adapter.on_successful_send(sent));
      old_observations.push_back({ sent.extended_sequence, packet_status_e::received, 9020000 + sent.send_time_us });
    }
    const auto reset = adapter.on_feedback(report(2, 30000, old_observations, 9026000, 2));
    ASSERT_EQ(reset.result, report_result_e::accepted);
    EXPECT_EQ(reset.changes.size(), 8U);
    EXPECT_EQ(reset.changes.front().sent.probe.cluster_id, old_cluster.cluster_id);
    EXPECT_EQ(adapter.snapshot().stale_probe_feedback_suppressed, 8U);
    EXPECT_EQ(adapter.snapshot().native_probe_successes, 0U);
    EXPECT_EQ(adapter.snapshot().native_probe_failures, 0U);
    EXPECT_EQ(adapter.ledger_snapshot().received_packets, 9U);
    const auto new_clusters = adapter.take_probe_requests();
    ASSERT_FALSE(new_clusters.empty());
    const auto new_cluster = new_clusters.front();
    ASSERT_GT(new_cluster.cluster_id, old_cluster.cluster_id);
    std::vector<packet_observation_t> observations;
    for (int index = 0; index < 8; ++index) {
      auto sent = packet(index + 9, 31000 + index * 500);
      sent.probe = { new_cluster.cluster_id, 5, 6400, static_cast<std::int32_t>(new_cluster.target_kbps) };
      ASSERT_TRUE(adapter.on_successful_send(sent));
      observations.push_back({ sent.extended_sequence, packet_status_e::received, 9020000 + sent.send_time_us });
    }
    ASSERT_EQ(adapter.on_feedback(report(3, 60000, observations, 9056000, 2)).result, report_result_e::accepted);
    EXPECT_GT(adapter.snapshot().native_probe_successes, 0U);
    EXPECT_EQ(adapter.snapshot().last_native_probe_cluster, new_cluster.cluster_id);
    EXPECT_GT(adapter.snapshot().last_native_probe_bps, 0);
    EXPECT_EQ(adapter.snapshot().stale_probe_feedback_suppressed, 8U);
    EXPECT_EQ(adapter.ledger_snapshot().received_packets, 17U);
  }

  // Synthetic work-conserving bottleneck, not a live stream or V6 acceptance.
  // Generated IP traffic follows the estimator target. Probe requests are
  // observed but not transmitted; real scheduling is a separate M3 task.
  TEST(GoogCcInput, NativeLossRecoveryPaddingContractUsesExplicitTransportMode) {
    std::int64_t padding_max[2] {};
    std::uint64_t recovered_requests[2] {};
    std::ofstream trace("googcc-padding-contract.csv");
    ASSERT_TRUE(trace);
    trace << "without_padding,time_us,target_kbps,requested_padding_kbps,network_loss_ppm,network_rtt_us,application_limited,exported_requests\n";
    for (int mode = 0; mode < 2; ++mode) {
      auto config = configuration();
      config.initial_kbps = config.maximum_kbps = 10000;
      config.periodic_alr_probing = true;
      config.loss_recovery_without_padding = mode != 0;
      session_t adapter(config);
      adapter.take_probe_requests();
      std::uint64_t sequence = 0, reports = 0, received = 0, missing = 0;
      for (std::int64_t at = 0; at < 40000000; at += 100000) {
        ASSERT_TRUE(adapter.process_interval(at));
        std::vector<packet_observation_t> observations;
        // Identical low-production frames in both modes, independent of the
        // estimator target. No synthetic padding or probes are submitted.
        for (int index = 0; index < 5; ++index) {
          auto sent = packet(sequence++, at);
          sent.ip_bytes = 1436;
          ASSERT_TRUE(adapter.on_successful_send(sent));
          const bool dropped = at >= 5000000 && at < 11000000 && index < 3;
          observations.push_back({ sent.extended_sequence, dropped ? packet_status_e::missing : packet_status_e::received,
            dropped ? -1 : 9000000000 + at + 20000 + index * 600 });
          received += !dropped;
          missing += dropped;
        }
        ASSERT_EQ(adapter.on_feedback(report(++reports, at + 50000, observations, 9000000000 + at + 40000)).result,
          report_result_e::accepted);
        const auto requests = adapter.take_probe_requests();
        if (at >= 11000000) recovered_requests[mode] += requests.size();
        padding_max[mode] = std::max(padding_max[mode], adapter.snapshot().requested_padding_kbps);
        trace << mode << ',' << at << ',' << adapter.snapshot().target_kbps << ',' << adapter.snapshot().requested_padding_kbps << ','
              << adapter.snapshot().network_loss_ppm << ',' << adapter.snapshot().network_rtt_us << ',' << adapter.snapshot().application_limited
              << ',' << requests.size() << '\n';
        EXPECT_EQ(adapter.snapshot().loss_recovery_without_padding, mode != 0);
      }
      EXPECT_EQ(adapter.ledger_snapshot().committed_packets, sequence);
      EXPECT_EQ(adapter.ledger_snapshot().received_packets, received);
      EXPECT_EQ(adapter.ledger_snapshot().missing_declarations, missing);
      EXPECT_EQ(adapter.snapshot().native_probe_successes, 0U);
      EXPECT_EQ(adapter.snapshot().native_probe_failures, 0U);
    }
    EXPECT_GT(padding_max[0], 0);
    EXPECT_EQ(padding_max[1], 0);
    EXPECT_EQ(recovered_requests[0], 0U);
    EXPECT_GT(recovered_requests[1], 0U);
  }

  TEST(GoogCcInput, CapacityStepReplayRespondsToQueueGrowth) {
    session_t adapter(configuration());
    struct queued_packet_t {
      sent_packet_t sent;
      double remaining_bytes;
    };
    struct delivery_t {
      std::int64_t due_us;
      packet_observation_t observation;
    };
    struct queued_report_t {
      std::int64_t due_us;
      std::int64_t receiver_sample_us;
      std::uint64_t sequence;
      std::vector<packet_observation_t> observations;
    };
    std::deque<queued_packet_t> bottleneck;
    std::deque<delivery_t> deliveries;
    std::deque<queued_report_t> reports;
    std::vector<packet_observation_t> ready;
    std::uint64_t next_packet = 0, next_report = 0, actually_received = 0, actually_dropped = 0;
    double credit = 0;
    std::int64_t before_step_target = 0, reduced_target = 0;
    constexpr std::int64_t receiver_offset = 9000000000;
    std::ofstream trace("googcc-capacity-trace.csv");
    ASSERT_TRUE(trace);
    trace << "time_ms,capacity_kbps,target_kbps,pacing_kbps,queued_ip_bytes,in_flight_ip_bytes\n";
    for (std::int64_t time_us = 0; time_us <= 16000000; time_us += 1000) {
      while (!reports.empty() && reports.front().due_us <= time_us) {
        auto message = std::move(reports.front());
        reports.pop_front();
        ASSERT_EQ(adapter.on_feedback(report(message.sequence, time_us, message.observations,
                                        message.receiver_sample_us))
                    .result,
          report_result_e::accepted);
      }
      if (time_us % 25000 == 0) {
        ASSERT_TRUE(adapter.process_interval(time_us));
        adapter.take_probe_requests();
      }
      if (time_us < 15000000) {
        credit += static_cast<double>(adapter.snapshot().target_kbps) / 8;
        while (credit >= 1280) {
          auto sent = packet(next_packet++, time_us);
          sent.kind = sent.extended_sequence % 6 == 5 ? packet_kind_e::fec : packet_kind_e::data;
          ASSERT_TRUE(adapter.on_successful_send(sent));
          bottleneck.push_back({ sent, 1280 });
          credit -= 1280;
        }
      }
      const int capacity_kbps = time_us < 5000000 ? 40000 : time_us < 10000000 ? 8000 :
                                                                                 25000;
      double service_bytes = static_cast<double>(capacity_kbps) / 8;
      while (service_bytes > 0 && !bottleneck.empty()) {
        auto &head = bottleneck.front();
        const auto used = std::min(service_bytes, head.remaining_bytes);
        service_bytes -= used;
        head.remaining_bytes -= used;
        if (head.remaining_bytes == 0) {
          const bool dropped = head.sent.extended_sequence % 100 == 17;
          actually_dropped += dropped;
          actually_received += !dropped;
          deliveries.push_back({ time_us + 20000, { head.sent.extended_sequence,
                                                    dropped ? packet_status_e::missing : packet_status_e::received,
                                                    dropped ? -1 : time_us + 20000 + receiver_offset } });
          bottleneck.pop_front();
        }
      }
      while (!deliveries.empty() && deliveries.front().due_us <= time_us) {
        ready.push_back(deliveries.front().observation);
        deliveries.pop_front();
      }
      if (time_us % 50000 == 0 && !ready.empty()) {
        reports.push_back({ time_us + 20000, time_us + receiver_offset, ++next_report, std::move(ready) });
        ready.clear();
      }
      if (time_us == 4750000) {
        before_step_target = adapter.snapshot().target_kbps;
      }
      if (time_us == 9000000) {
        reduced_target = adapter.snapshot().target_kbps;
      }
      if (time_us % 250000 == 0) {
        double queued_bytes = 0;
        for (const auto &entry : bottleneck) {
          queued_bytes += entry.remaining_bytes;
        }
        trace << time_us / 1000 << ',' << capacity_kbps << ',' << adapter.snapshot().target_kbps << ','
              << adapter.snapshot().pacing_kbps << ',' << static_cast<std::uint64_t>(queued_bytes) << ','
              << adapter.ledger_snapshot().data_in_flight_bytes << '\n';
      }
    }
    ASSERT_TRUE(bottleneck.empty());
    ASSERT_TRUE(deliveries.empty());
    ASSERT_TRUE(reports.empty());
    EXPECT_EQ(adapter.ledger_snapshot().committed_packets, next_packet);
    EXPECT_EQ(adapter.ledger_snapshot().received_packets, actually_received);
    EXPECT_EQ(adapter.ledger_snapshot().missing_declarations, actually_dropped);
    EXPECT_EQ(adapter.ledger_snapshot().data_in_flight_bytes, 0);
    EXPECT_GT(before_step_target, 8000);
    EXPECT_LT(reduced_target, before_step_target);
    EXPECT_GT(reduced_target, 0);
  }

  TEST(GoogCcInput, FullUint64WireIdentityUsesIndependentSignedControllerSequence) {
    session_t adapter(configuration());
    constexpr auto sequence = static_cast<std::uint64_t>(INT64_MAX) + 1234;
    ASSERT_TRUE(adapter.on_successful_send(packet(sequence, 0)));
    ASSERT_TRUE(adapter.on_successful_send(packet(sequence + 1, 100)));
    const std::array initial {
      packet_observation_t { sequence, packet_status_e::missing, -1 },
      packet_observation_t { sequence + 1, packet_status_e::received, 9000500 }
    };
    ASSERT_EQ(adapter.on_feedback(report(1, 1000, initial, 9001000)).result, report_result_e::accepted);
    const std::array late { packet_observation_t { sequence, packet_status_e::received, 9001500 } };
    ASSERT_EQ(adapter.on_feedback(report(2, 2000, late, 9002000)).changes.size(), 1u);
    EXPECT_EQ(adapter.snapshot().accepted_sends, 2u);
    EXPECT_EQ(adapter.snapshot().feedback_packet_changes, 3u);
    EXPECT_EQ(adapter.snapshot().unmapped_feedback_changes, 0u);
    EXPECT_EQ(adapter.ledger_snapshot().late_corrections, 1u);
  }

  TEST(GoogCcInput, EvictedControllerMappingRemainsUnknownWithoutChangingAuthoritativeLoss) {
    auto config = configuration();
    config.history_capacity = 2;
    session_t adapter(config);
    ASSERT_TRUE(adapter.on_successful_send(packet(1, 0)));
    ASSERT_TRUE(adapter.on_successful_send(packet(2, 100)));
    ASSERT_TRUE(adapter.on_successful_send(packet(3, 200)));
    const std::array expired { packet_observation_t { 1, packet_status_e::received, 9000400 } };
    ASSERT_EQ(adapter.on_feedback(report(1, 1000, expired, 9001000)).changes.size(), 1u);
    EXPECT_EQ(adapter.snapshot().sequence_mapping_size, 2u);
    EXPECT_EQ(adapter.snapshot().sequence_mapping_evictions, 1u);
    EXPECT_EQ(adapter.snapshot().unmapped_feedback_changes, 1u);
    EXPECT_EQ(adapter.snapshot().feedback_batches, 0u);
    EXPECT_EQ(adapter.snapshot().last_accepted_feedback_us, 1000);
    EXPECT_EQ(adapter.snapshot().last_covered_feedback_us, -1);
    EXPECT_EQ(adapter.ledger_snapshot().received_packets, 1u);
    EXPECT_EQ(adapter.ledger_snapshot().missing_declarations, 0u);
    EXPECT_EQ(adapter.ledger_snapshot().data_in_flight_bytes, 2560u);
  }

  TEST(GoogCcInput, EventRedeliveryAndUnsubmittedCoverageCannotManufactureNewObservations) {
    session_t adapter(configuration());
    const auto send_event = adapter.wire.commit_success_event(packet(5, 0));
    ASSERT_TRUE(send_event);
    ASSERT_TRUE(adapter.controller.on_successful_send(*send_event));
    EXPECT_FALSE(adapter.controller.on_successful_send(*send_event));
    const std::array observations { packet_observation_t { 5, packet_status_e::received, 9000500 } };
    const auto event = adapter.wire.apply_wire_event(encode(report(1, 1000, observations, 9001000)), 1000);
    ASSERT_TRUE(adapter.controller.on_feedback(event));
    EXPECT_FALSE(adapter.controller.on_feedback(event));
    EXPECT_EQ(adapter.snapshot().feedback_batches, 1u);
    const std::array unknown { packet_observation_t { 6, packet_status_e::received, 9001500 } };
    EXPECT_TRUE(adapter.on_feedback(report(2, 2000, unknown, 9002000)).changes.empty());
    EXPECT_EQ(adapter.snapshot().feedback_packet_changes, 1u);
    EXPECT_EQ(adapter.snapshot().last_accepted_feedback_us, 2000);
    EXPECT_EQ(adapter.snapshot().last_covered_feedback_us, 1000);
    EXPECT_EQ(adapter.ledger_snapshot().committed_packets, 1u);
    EXPECT_EQ(adapter.ledger_snapshot().received_packets, 1u);
    EXPECT_EQ(adapter.ledger_snapshot().unmatched_observations, 1u);
  }

  TEST(GoogCcInput, LateOwnerDeliveryPreservesActualSubmissionTimeAndRejectsStaleInput) {
    session_t adapter(configuration());
    const auto send_event = adapter.wire.commit_success_event(packet(1, 100));
    ASSERT_TRUE(send_event);
    ASSERT_TRUE(adapter.process_interval(200));
    EXPECT_FALSE(adapter.controller.on_successful_send(*send_event));
    EXPECT_EQ(send_event->sent.send_time_us, 100);
    EXPECT_EQ(adapter.snapshot().accepted_sends, 0u);
    EXPECT_EQ(adapter.snapshot().sequence_mapping_size, 0u);
    const std::array observations { packet_observation_t { 1, packet_status_e::received, 9000200 } };
    const auto event = adapter.wire.apply_wire_event(encode(report(1, 300, observations, 9000300)), 300);
    ASSERT_TRUE(adapter.process_interval(400));
    EXPECT_FALSE(adapter.controller.on_feedback(event));
    EXPECT_EQ(event.processing_time_us, 300);
    EXPECT_EQ(event.feedback.changes.front().sent.send_time_us, 100);
    EXPECT_EQ(adapter.snapshot().feedback_packet_changes, 0u);
    EXPECT_EQ(adapter.ledger_snapshot().received_packets, 1u);
  }

  TEST(GoogCcInput, SignedControllerSequenceExhaustionFailsClosedWithoutWrapping) {
    auto config = configuration();
    config.first_cc_sequence = INT64_MAX - 1;
    session_t adapter(config);
    ASSERT_TRUE(adapter.on_successful_send(packet(1, 0)));
    EXPECT_FALSE(adapter.on_successful_send(packet(2, 100)));
    EXPECT_EQ(adapter.snapshot().accepted_sends, 1u);
    EXPECT_EQ(adapter.snapshot().rejected_send_events, 1u);
    EXPECT_EQ(adapter.snapshot().sequence_mapping_size, 1u);
    EXPECT_EQ(adapter.ledger_snapshot().committed_packets, 2u);
    config.first_cc_sequence = INT64_MAX;
    EXPECT_THROW(googcc_adapter_t { config }, std::invalid_argument);
  }

  TEST(GoogCcInput, InvalidOwnedEventShapeIsRejectedBeforeControllerStateChanges) {
    session_t adapter(configuration());
    ASSERT_TRUE(adapter.on_successful_send(packet(1, 100)));
    const std::array observations { packet_observation_t { 1, packet_status_e::received, 9000200 } };
    const auto event = adapter.wire.apply_wire_event(encode(report(1, 300, observations, 9000300)), 300);
    ASSERT_EQ(event.feedback.changes.size(), 1u);
    auto invalid = event;
    invalid.connection_epoch = 8;
    EXPECT_FALSE(adapter.controller.on_feedback(invalid));
    invalid = event;
    invalid.receiver_sample_time_us = -1;
    EXPECT_FALSE(adapter.controller.on_feedback(invalid));
    invalid = event;
    invalid.feedback.changes.front().first_arrival_us = 9000400;
    EXPECT_FALSE(adapter.controller.on_feedback(invalid));
    invalid = event;
    invalid.feedback.changes.resize(send_ledger_t::max_report_packets + 1);
    EXPECT_FALSE(adapter.controller.on_feedback(invalid));
    EXPECT_EQ(adapter.snapshot().feedback_packet_changes, 0u);
    EXPECT_EQ(adapter.snapshot().last_accepted_feedback_us, -1);
    ASSERT_TRUE(adapter.controller.on_feedback(event));
    EXPECT_EQ(adapter.snapshot().feedback_packet_changes, 1u);
    EXPECT_EQ(adapter.ledger_snapshot().received_packets, 1u);
  }
}  // namespace
