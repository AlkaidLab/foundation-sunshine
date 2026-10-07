/**
 * @file src/transport/transport_feedback.h
 * @brief Bounded successful-send ledger and idempotent packet feedback.
 */
#pragma once

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace transport {
  enum class packet_kind_e : std::uint8_t { data,
    fec,
    repair,
    probe };
  enum class packet_status_e : std::uint8_t { pending,
    received,
    missing,
    unknown };
  enum class protection_class_e : std::uint8_t { base,
    key,
    recovery };

  struct packet_protection_t {
    std::uint16_t data_shards = 0;
    std::uint16_t total_shards = 0;
    std::uint16_t shard_index = 0;
    std::uint8_t block_index = 0;
    protection_class_e frame_class = protection_class_e::base;
  };

  struct probe_info_t {
    std::int32_t cluster_id = -1;
    std::int32_t min_packets = 0;
    std::int32_t min_bytes = 0;
    std::int32_t send_kbps = 0;
  };

  struct sent_packet_t {
    std::uint64_t extended_sequence = 0;
    std::int64_t send_time_us = 0;
    std::uint32_t ip_bytes = 0;
    std::uint64_t frame_id = 0;
    std::uint64_t policy_revision = 0;
    packet_kind_e kind = packet_kind_e::data;
    probe_info_t probe;
    packet_protection_t protection {};
  };

  struct packet_observation_t {
    std::uint64_t extended_sequence = 0;
    packet_status_e status = packet_status_e::unknown;
    // Receiver clock; only received packets carry a nonnegative first arrival.
    std::int64_t first_arrival_us = -1;
  };

  struct packet_report_t {
    std::uint64_t connection_epoch = 0;
    std::uint64_t report_sequence = 0;
    std::uint64_t receiver_clock_epoch = 0;
    std::int64_t received_at_us = 0;  // Sender's monotonic clock.
    std::span<const packet_observation_t> packets;
    std::int64_t receiver_sample_time_us = -1;
  };

  struct feedback_change_t {
    sent_packet_t sent;
    packet_status_e status = packet_status_e::unknown;
    std::int64_t first_arrival_us = -1;
    bool first_missing = false;
    bool first_recovered = false;
  };

  enum class report_result_e { accepted,
    duplicate,
    too_old,
    wrong_epoch,
    invalid };

  struct feedback_result_t {
    report_result_e result = report_result_e::invalid;
    bool receiver_clock_changed = false;
    std::size_t unmatched_packets = 0;
    std::vector<feedback_change_t> changes;
  };

  // Owned, authoritative events. The wire ledger assigns one increasing event
  // sequence across successful sends and accepted reports while holding its
  // lock. A controller owner must consume them promptly and in that order.
  // No packet state or caller-owned span crosses this boundary.
  struct successful_send_event_t {
    std::uint64_t connection_epoch = 0;
    std::uint64_t event_sequence = 0;
    sent_packet_t sent;
    std::uint64_t data_in_flight_bytes = 0;
    std::uint64_t commit_ordinal = 0;
  };

  struct feedback_event_t {
    std::uint64_t connection_epoch = 0;
    std::uint64_t event_sequence = 0;  // Zero for a rejected/non-new report.
    std::uint64_t receiver_clock_epoch = 0;
    std::int64_t receiver_sample_time_us = -1;
    std::int64_t processing_time_us = -1;
    std::uint64_t data_in_flight_bytes = 0;
    // Bounded by send_ledger_t::max_report_packets; wire is stricter (256).
    feedback_result_t feedback;
  };

  struct send_ledger_snapshot_t {
    std::uint64_t committed_packets = 0;
    std::uint64_t committed_ip_bytes = 0;
    std::uint64_t received_packets = 0;
    std::uint64_t received_ip_bytes = 0;
    std::uint64_t missing_declarations = 0;
    std::uint64_t late_corrections = 0;
    std::uint64_t unmatched_observations = 0;
    std::uint64_t history_evictions = 0;
    std::uint64_t unresolved_evictions = 0;
    std::uint64_t data_in_flight_bytes = 0;
    bool counters_valid = true;
  };

  // One current view of mature, actually submitted packets. Unknown/pending
  // coverage is explicit; reserved-but-unsent identities never enter counts.
  struct network_window_t {
    std::uint64_t connection_epoch = 0;
    std::uint64_t receiver_clock_epoch = 0;
    std::int64_t sampled_at_us = -1;
    std::int64_t begins_at_us = -1;
    std::int64_t ends_at_us = -1;
    std::uint32_t received = 0;
    std::uint32_t missing = 0;
    std::uint32_t unknown = 0;
    bool valid = false;
    bool history_truncated = false;
  };

  struct wire_feedback_snapshot_t {
    send_ledger_snapshot_t ledger;
    bool negotiated = false;
    std::uint64_t accepted_reports = 0;
    std::uint64_t rejected_reports = 0;
    std::uint64_t rate_limited_reports = 0;
    std::uint64_t submitted_through_exclusive = 0;
    std::int64_t last_feedback_us = -1;
    std::int64_t last_new_feedback_us = -1;
    std::int64_t latest_covered_send_us = -1;
  };

  struct network_statistics_t {
    wire_feedback_snapshot_t cumulative;
    network_window_t window;
  };

  // One session/flow and one serialized owner. Call commit_success only after
  // successful submission, using that submission's timestamp and IP size.
  // Sequence gaps, expired history and unknown coverage never manufacture loss.
  class send_ledger_t {
  public:
    static constexpr std::size_t report_history = 512;
    static constexpr std::size_t max_report_packets = 1024;
    static constexpr std::size_t max_capacity = 1U << 20;
    static constexpr std::size_t max_network_window_samples = 32768;

    explicit send_ledger_t(std::uint64_t connection_epoch, std::size_t capacity = 16384);
    bool
    commit_success(const sent_packet_t &packet);
    feedback_result_t
    apply(const packet_report_t &report);
    void
    expire_before(std::int64_t send_time_us);
    std::optional<sent_packet_t>
    find(std::uint64_t sequence) const;
    const send_ledger_snapshot_t &
    snapshot() const noexcept;
    network_window_t
    network_window(std::int64_t now_us, std::int64_t horizon_us = 2000000,
      std::int64_t maturity_us = 200000, std::size_t maximum_samples = max_network_window_samples) const;
    std::size_t
    size() const noexcept;

  private:
    struct entry_t {
      sent_packet_t packet;
      packet_status_e status = packet_status_e::pending;
      std::int64_t first_arrival_us = -1;
      std::uint64_t receiver_clock_epoch = 0;
    };

    void
    evict_oldest();
    void
    add_counter(std::uint64_t &counter, std::uint64_t value);

    const std::uint64_t connection_epoch_;
    const std::size_t capacity_;
    std::unordered_map<std::uint64_t, entry_t> entries_;
    std::deque<std::uint64_t> order_;
    std::optional<std::uint64_t> last_sent_sequence_;
    std::int64_t last_sent_time_us_ = 0;
    std::int64_t latest_evicted_send_us_ = -1;
    std::uint64_t highest_report_sequence_ = 0;
    std::bitset<report_history> seen_reports_;
    std::uint64_t receiver_clock_epoch_ = 0;
    std::int64_t last_report_received_us_ = 0;
    send_ledger_snapshot_t snapshot_;
  };
}  // namespace transport
