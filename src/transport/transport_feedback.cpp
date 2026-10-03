#include "transport_feedback.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace transport {
  send_ledger_t::send_ledger_t(std::uint64_t connection_epoch, std::size_t capacity):
      connection_epoch_(connection_epoch), capacity_(capacity) {
    if (!connection_epoch || !capacity || capacity > max_capacity) {
      throw std::invalid_argument("Invalid transport ledger epoch or capacity");
    }
    entries_.reserve(capacity);
  }

  void
  send_ledger_t::add_counter(std::uint64_t &counter, std::uint64_t value) {
    if (value > std::numeric_limits<std::uint64_t>::max() - counter) {
      snapshot_.counters_valid = false;
      counter = std::numeric_limits<std::uint64_t>::max();
    }
    else {
      counter += value;
    }
  }

  bool
  send_ledger_t::commit_success(const sent_packet_t &packet) {
    const auto &protection = packet.protection;
    if (protection.data_shards) {
      if (protection.data_shards > 1023 || protection.total_shards < protection.data_shards ||
          protection.total_shards > 1023 ||
          (protection.total_shards > protection.data_shards && protection.total_shards > 255) ||
          protection.shard_index >= protection.total_shards || protection.block_index >= 4 ||
          protection.frame_class > protection_class_e::recovery ||
          packet.kind != (protection.shard_index < protection.data_shards ? packet_kind_e::data : packet_kind_e::fec)) return false;
      if (protection.frame_data_shards || protection.frame_blocks) {
        if (!protection.frame_blocks || protection.frame_blocks > 4 ||
            protection.frame_data_shards < protection.frame_blocks || protection.frame_data_shards > 4092 ||
            protection.block_index >= protection.frame_blocks ||
            protection.data_shards != protection.frame_data_shards / protection.frame_blocks +
                                        (protection.block_index < protection.frame_data_shards % protection.frame_blocks)) return false;
      }
    }
    else if (protection.total_shards || protection.shard_index || protection.block_index ||
             protection.frame_class != protection_class_e::base || protection.frame_data_shards || protection.frame_blocks)
      return false;
    if (packet.send_time_us < 0 || packet.ip_bytes < 28 || packet.ip_bytes > 65575 ||
        (last_sent_sequence_ && (packet.extended_sequence <= *last_sent_sequence_ ||
                                  packet.send_time_us < last_sent_time_us_)) ||
        packet.kind > packet_kind_e::probe || packet.probe.cluster_id < -1 ||
        (packet.probe.cluster_id >= 0 && (packet.probe.min_packets <= 0 || packet.probe.min_bytes <= 0 ||
                                           packet.probe.send_kbps <= 0)) ||
        (packet.probe.cluster_id == -1 && (packet.probe.min_packets || packet.probe.min_bytes || packet.probe.send_kbps))) {
      return false;
    }
    // Insert before eviction so a failed allocation does not erase history.
    auto [iterator, inserted] = entries_.emplace(packet.extended_sequence, entry_t { packet });
    if (!inserted) {
      return false;
    }
    try {
      order_.push_back(packet.extended_sequence);
    }
    catch (...) {
      entries_.erase(iterator);
      throw;
    }
    add_counter(snapshot_.committed_packets, 1);
    iterator->second.commit_ordinal = snapshot_.committed_packets;
    add_counter(snapshot_.committed_ip_bytes, packet.ip_bytes);
    snapshot_.data_in_flight_bytes += packet.ip_bytes;
    last_sent_sequence_ = packet.extended_sequence;
    last_sent_time_us_ = packet.send_time_us;
    if (order_.size() > capacity_) {
      evict_oldest();
    }
    return true;
  }

  feedback_result_t
  send_ledger_t::apply(const packet_report_t &report) {
    feedback_result_t result;
    if (report.connection_epoch != connection_epoch_) {
      result.result = report_result_e::wrong_epoch;
      return result;
    }
    if (!report.report_sequence || !report.receiver_clock_epoch || report.received_at_us < 0 ||
        report.received_at_us < last_report_received_us_ ||
        report.packets.size() > max_report_packets || report.receiver_sample_time_us < -1 ||
        (receiver_clock_epoch_ && report.receiver_clock_epoch > receiver_clock_epoch_ &&
          report.report_sequence <= highest_report_sequence_)) {
      return result;
    }
    if (report.receiver_clock_epoch < receiver_clock_epoch_) {
      result.result = report_result_e::wrong_epoch;
      return result;
    }
    if (report.report_sequence <= highest_report_sequence_) {
      const auto distance = highest_report_sequence_ - report.report_sequence;
      if (distance >= report_history) {
        result.result = report_result_e::too_old;
        return result;
      }
      if (seen_reports_[distance]) {
        result.result = report_result_e::duplicate;
        return result;
      }
    }

    // Validate the whole report before changing the replay window or counters.
    std::optional<std::uint64_t> previous_sequence;
    for (const auto &observation : report.packets) {
      if (observation.status > packet_status_e::unknown ||
          (previous_sequence && observation.extended_sequence <= *previous_sequence) ||
          (observation.status == packet_status_e::received ? observation.first_arrival_us < 0 ||
                                                               (report.receiver_sample_time_us >= 0 && observation.first_arrival_us > report.receiver_sample_time_us) :
                                                             observation.first_arrival_us != -1)) {
        return result;
      }
      previous_sequence = observation.extended_sequence;
      const auto iterator = entries_.find(observation.extended_sequence);
      if (iterator == entries_.end()) {
        continue;
      }
      const auto &entry = iterator->second;
      if (entry.packet.send_time_us > report.received_at_us ||
          (entry.status == packet_status_e::received && observation.status == packet_status_e::received &&
            entry.receiver_clock_epoch == report.receiver_clock_epoch &&
            entry.first_arrival_us != observation.first_arrival_us)) {
        return result;
      }
    }
    result.changes.reserve(report.packets.size());
    if (report.report_sequence > highest_report_sequence_) {
      const auto distance = report.report_sequence - highest_report_sequence_;
      if (distance >= report_history) {
        seen_reports_.reset();
      }
      else {
        seen_reports_ <<= distance;
      }
      highest_report_sequence_ = report.report_sequence;
    }
    seen_reports_.set(highest_report_sequence_ - report.report_sequence);
    result.receiver_clock_changed = receiver_clock_epoch_ && receiver_clock_epoch_ != report.receiver_clock_epoch;
    receiver_clock_epoch_ = report.receiver_clock_epoch;
    last_report_received_us_ = report.received_at_us;
    result.result = report_result_e::accepted;

    for (const auto &observation : report.packets) {
      auto iterator = entries_.find(observation.extended_sequence);
      if (iterator == entries_.end()) {
        ++result.unmatched_packets;
        add_counter(snapshot_.unmatched_observations, 1);
        continue;
      }
      auto &entry = iterator->second;
      if (observation.status == packet_status_e::received && entry.status != packet_status_e::received) {
        const bool recovered = entry.status == packet_status_e::missing;
        if (recovered) {
          add_counter(snapshot_.late_corrections, 1);
          entry.late_correction = true;
        }
        else {
          snapshot_.data_in_flight_bytes -= entry.packet.ip_bytes;
        }
        entry.status = packet_status_e::received;
        entry.first_arrival_us = observation.first_arrival_us;
        entry.receiver_clock_epoch = report.receiver_clock_epoch;
        add_counter(snapshot_.received_packets, 1);
        add_counter(snapshot_.received_ip_bytes, entry.packet.ip_bytes);
        result.changes.push_back({ entry.packet, entry.status, entry.first_arrival_us, false, recovered });
      }
      else if (observation.status == packet_status_e::missing && entry.status == packet_status_e::pending) {
        entry.status = packet_status_e::missing;
        entry.receiver_clock_epoch = report.receiver_clock_epoch;
        snapshot_.data_in_flight_bytes -= entry.packet.ip_bytes;
        add_counter(snapshot_.missing_declarations, 1);
        result.changes.push_back({ entry.packet, entry.status, -1, true, false });
      }
    }
    return result;
  }

  void
  send_ledger_t::evict_oldest() {
    auto iterator = entries_.find(order_.front());
    latest_evicted_send_us_ = iterator->second.packet.send_time_us;
    if (iterator->second.status == packet_status_e::pending) {
      snapshot_.data_in_flight_bytes -= iterator->second.packet.ip_bytes;
      add_counter(snapshot_.unresolved_evictions, 1);
    }
    add_counter(snapshot_.history_evictions, 1);
    entries_.erase(iterator);
    order_.pop_front();
  }

  void
  send_ledger_t::expire_before(std::int64_t send_time_us) {
    while (!order_.empty() && entries_.at(order_.front()).packet.send_time_us < send_time_us) {
      evict_oldest();
    }
  }

  std::optional<sent_packet_t>
  send_ledger_t::find(std::uint64_t sequence) const {
    const auto iterator = entries_.find(sequence);
    return iterator == entries_.end() ? std::nullopt : std::optional(iterator->second.packet);
  }

  const send_ledger_snapshot_t &
  send_ledger_t::snapshot() const noexcept {
    return snapshot_;
  }

  std::size_t
  send_ledger_t::size() const noexcept {
    return entries_.size();
  }

  network_window_t
  send_ledger_t::network_window(std::int64_t now_us, std::int64_t horizon_us,
    std::int64_t maturity_us, std::size_t maximum_samples) const {
    network_window_t result;
    result.connection_epoch = connection_epoch_;
    result.receiver_clock_epoch = receiver_clock_epoch_;
    result.sampled_at_us = now_us;
    if (now_us < 0 || horizon_us <= 0 || horizon_us > 5000000 || maturity_us < 0 ||
        maturity_us >= horizon_us || now_us < maturity_us || !maximum_samples || maximum_samples > max_network_window_samples ||
        !snapshot_.counters_valid) return result;
    result.begins_at_us = now_us >= horizon_us ? now_us - horizon_us : 0;
    result.ends_at_us = now_us - maturity_us;
    result.history_truncated = latest_evicted_send_us_ >= result.begins_at_us;
    std::size_t count = 0;
    for (auto item = order_.rbegin(); item != order_.rend(); ++item) {
      const auto &entry = entries_.at(*item);
      if (entry.packet.send_time_us > result.ends_at_us) continue;
      if (entry.packet.send_time_us < result.begins_at_us) break;
      if (count == maximum_samples) { result.history_truncated = true; break; }
      ++count;
      const auto status = entry.receiver_clock_epoch == receiver_clock_epoch_ ? entry.status : packet_status_e::unknown;
      if (status == packet_status_e::received) ++result.received;
      else if (status == packet_status_e::missing) ++result.missing;
      else ++result.unknown;
    }
    result.valid = true;
    return result;
  }

  protection_trace_t
  send_ledger_t::protection_trace(std::int64_t now_us, std::int64_t horizon_us,
    std::int64_t maturity_us, std::size_t maximum_samples) const {
    protection_trace_t trace;
    trace.connection_epoch = connection_epoch_;
    trace.receiver_clock_epoch = receiver_clock_epoch_;
    trace.sampled_at_us = now_us;
    trace.last_feedback_us = highest_report_sequence_ ? last_report_received_us_ : -1;
    if (now_us < 0 || horizon_us <= 0 || horizon_us > 5000000 || maturity_us < 0 ||
        maturity_us >= horizon_us || now_us < maturity_us || !maximum_samples || maximum_samples > 4096 ||
        !snapshot_.counters_valid) return trace;
    trace.settled_until_us = now_us - maturity_us;
    const auto beginning = now_us >= horizon_us ? now_us - horizon_us : 0;
    trace.samples.reserve(std::min(maximum_samples, order_.size()));
    for (auto item = order_.rbegin(); item != order_.rend(); ++item) {
      const auto &entry = entries_.at(*item);
      if (entry.packet.send_time_us > trace.settled_until_us) continue;
      if (entry.packet.send_time_us < beginning) break;
      if (trace.samples.size() == maximum_samples) {
        trace.history_truncated = true;
        break;
      }
      const auto status = entry.receiver_clock_epoch == receiver_clock_epoch_ ? entry.status : packet_status_e::unknown;
      trace.samples.push_back({ entry.packet, status, entry.late_correction, entry.commit_ordinal });
    }
    std::reverse(trace.samples.begin(), trace.samples.end());
    trace.valid = true;
    return trace;
  }
}  // namespace transport
