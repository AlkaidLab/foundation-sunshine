#include "transport_feedback_wire.h"

#include <algorithm>
#include <array>
#include <limits>

namespace transport {
  protection_trace_t
  wire_feedback_t::protection_trace(std::int64_t now_us, std::int64_t horizon_us,
    std::int64_t maturity_us, std::size_t maximum_samples) const {
    std::lock_guard lock(mutex_);
    return ledger_.protection_trace(now_us, horizon_us, maturity_us, maximum_samples);
  }

  wire_feedback_t::wire_feedback_t(std::uint64_t epoch, bool negotiated, std::size_t capacity, std::uint64_t initial_event_sequence):
      connection_epoch_(epoch), negotiated_(negotiated), ledger_(epoch, capacity), event_sequence_(initial_event_sequence) {
    state_.negotiated = negotiated;
  }

  bool
  wire_feedback_t::commit_success(const sent_packet_t &packet) {
    return commit_success_event(packet).has_value();
  }

  std::optional<successful_send_event_t>
  wire_feedback_t::commit_success_event(const sent_packet_t &packet) {
    std::lock_guard lock(mutex_);
    if (packet.extended_sequence == std::numeric_limits<std::uint64_t>::max() ||
        event_sequence_ == std::numeric_limits<std::uint64_t>::max() || !ledger_.commit_success(packet)) return std::nullopt;
    state_.submitted_through_exclusive = packet.extended_sequence + 1;
    return successful_send_event_t { connection_epoch_, ++event_sequence_, packet,
      ledger_.snapshot().data_in_flight_bytes, ledger_.snapshot().committed_packets };
  }

  feedback_result_t
  wire_feedback_t::apply_wire(std::span<const std::uint8_t> bytes, std::int64_t received_at_us) {
    return apply_wire_event(bytes, received_at_us).feedback;
  }

  feedback_event_t
  wire_feedback_t::apply_wire_event(std::span<const std::uint8_t> bytes, std::int64_t received_at_us) {
    std::lock_guard lock(mutex_);
    feedback_event_t event;
    event.connection_epoch = connection_epoch_;
    event.processing_time_us = received_at_us;
    event.data_in_flight_bytes = ledger_.snapshot().data_in_flight_bytes;
    if (!negotiated_ || received_at_us < 0 || received_at_us < last_input_us_ || bytes.size() > TF_MAX_REPORT_BYTES ||
        event_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
      ++state_.rejected_reports;
      return event;
    }
    // This cap includes a conservative bound for one nonfragmented encrypted
    // ENet datagram's overhead. Reject before parsing or allocating changes.
    if (last_input_us_ >= 0) {
      const auto elapsed = static_cast<std::uint64_t>(received_at_us - last_input_us_);
      const auto refill = elapsed >= 1000000 ? feedback_wire_bytes_per_second :
                                               elapsed * feedback_wire_bytes_per_second / 1000000;
      input_tokens_ = static_cast<std::uint32_t>(std::min<std::uint64_t>(feedback_burst_bytes, input_tokens_ + refill));
    }
    last_input_us_ = received_at_us;
    const auto wire_bytes = bytes.size() + report_wire_overhead_bound;
    if (wire_bytes > input_tokens_) {
      ++state_.rate_limited_reports;
      return event;
    }
    input_tokens_ -= static_cast<std::uint32_t>(wire_bytes);
    TF_PACKET_REPORT decoded;
    if (!TfDecodeReport(bytes.data(), bytes.size(), &decoded)) {
      ++state_.rejected_reports;
      return event;
    }
    std::array<packet_observation_t, TF_MAX_PACKETS> observations;
    for (std::size_t i = 0; i < decoded.packetCount; ++i) {
      observations[i] = { decoded.baseExtendedSequence + i, static_cast<packet_status_e>(decoded.status[i]),
        decoded.status[i] == TF_RECEIVED ? static_cast<std::int64_t>(decoded.firstArrivalTimeUs[i]) : -1 };
    }
    event.receiver_clock_epoch = decoded.receiverClockEpoch;
    event.receiver_sample_time_us = static_cast<std::int64_t>(decoded.receiverSampleTimeUs);
    event.feedback = ledger_.apply({ decoded.connectionEpoch, decoded.reportSequence,
      decoded.receiverClockEpoch, received_at_us,
      std::span(observations.data(), decoded.packetCount), event.receiver_sample_time_us });
    event.data_in_flight_bytes = ledger_.snapshot().data_in_flight_bytes;
    if (event.feedback.result == report_result_e::accepted) {
      event.event_sequence = ++event_sequence_;
      ++state_.accepted_reports;
      state_.last_feedback_us = received_at_us;
      if (event.feedback.receiver_clock_changed) {
        state_.last_new_feedback_us = -1;
        state_.latest_covered_send_us = -1;
      }
      if (!event.feedback.changes.empty()) {
        state_.last_new_feedback_us = received_at_us;
        for (const auto &change : event.feedback.changes)
          state_.latest_covered_send_us = std::max(state_.latest_covered_send_us, change.sent.send_time_us);
      }
    }
    else
      ++state_.rejected_reports;
    return event;
  }

  std::optional<TF_READY>
  wire_feedback_t::ready(std::int64_t sample) const {
    std::lock_guard lock(mutex_);
    if (!negotiated_ || sample < 0) return std::nullopt;
    return TF_READY { connection_epoch_, state_.submitted_through_exclusive,
      static_cast<std::uint64_t>(sample), 50, TF_MAX_PACKETS, feedback_wire_bytes_per_second };
  }

  wire_feedback_snapshot_t
  wire_feedback_t::snapshot() const {
    std::lock_guard lock(mutex_);
    auto result = state_;
    result.ledger = ledger_.snapshot();
    return result;
  }

  network_statistics_t
  wire_feedback_t::network_statistics(std::int64_t now_us) const {
    std::lock_guard lock(mutex_);
    auto cumulative = state_;
    cumulative.ledger = ledger_.snapshot();
    return { cumulative, ledger_.network_window(now_us) };
  }
}  // namespace transport
