#pragma once

#include "third-party/moonlight-common-c/src/TransportFeedbackWire.h"
#include "transport_feedback.h"

#include <mutex>
#include <span>

namespace transport {
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

  // Serializes UDP commits and authenticated control input for one video flow.
  // Transport negotiation does not grant automatic bitrate/FEC control.
  class wire_feedback_t {
  public:
    static constexpr std::uint32_t feedback_wire_bytes_per_second = 125000;
    static constexpr std::uint32_t report_wire_overhead_bound = 96;
    static constexpr std::uint32_t feedback_burst_bytes = 8 * (TF_MAX_REPORT_BYTES + report_wire_overhead_bound);
    // Nonzero initial event numbering permits deterministic exhaustion checks.
    // It does not restore ledger state or a prior connection epoch.
    wire_feedback_t(std::uint64_t connection_epoch, bool negotiated, std::size_t capacity = 16384,
      std::uint64_t initial_event_sequence = 0);
    // Compatibility entry points delegate to the event path, never apply twice.
    bool
    commit_success(const sent_packet_t &packet);
    feedback_result_t
    apply_wire(std::span<const std::uint8_t> bytes, std::int64_t received_at_us);
    std::optional<successful_send_event_t>
    commit_success_event(const sent_packet_t &packet);
    // Input must already have passed control-channel authentication. Returned
    // changes own all their data, including the original OS submission time.
    feedback_event_t
    apply_wire_event(std::span<const std::uint8_t> bytes, std::int64_t processing_time_us);
    std::optional<TF_READY>
    ready(std::int64_t sender_sample_us) const;
    wire_feedback_snapshot_t
    snapshot() const;
    // Counts, coverage epochs and freshness are copied under the same lock.
    network_statistics_t
    network_statistics(std::int64_t now_us) const;

  private:
    const std::uint64_t connection_epoch_;
    const bool negotiated_;
    mutable std::mutex mutex_;
    send_ledger_t ledger_;
    wire_feedback_snapshot_t state_;
    std::int64_t last_input_us_ = -1;
    std::uint32_t input_tokens_ = feedback_burst_bytes;
    std::uint64_t event_sequence_ = 0;
  };
}  // namespace transport
