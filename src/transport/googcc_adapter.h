/**
 * @file src/transport/googcc_adapter.h
 * @brief Isolated adapter to the pinned, unmodified WebRTC GoogCC algorithms.
 */
#pragma once

#include "transport_feedback.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace transport {
  struct googcc_config_t {
    std::uint64_t connection_epoch = 0;
    std::int64_t start_time_us = 0;
    int minimum_kbps = 1000;
    int maximum_kbps = 50000;
    int initial_kbps = 30000;
    std::size_t history_capacity = 16384;
    // Independent controller numbering, never a cast of the wire uint64 ID.
    // INT64_MAX is reserved so allocation cannot wrap; exhaustion fails closed.
    std::int64_t first_cc_sequence = 0;
    // Offset native cluster IDs into a per-flow, non-reused namespace across
    // explicit controller replacements. No packet history is duplicated.
    std::int32_t first_probe_cluster_id = 0;
    bool pacer_queue_feedback = false;
    bool queue_pushback = false;
    int queue_delay_ms = 100;
    bool periodic_alr_probing = false;
    // Native integration mode for transports without a padding protocol.
    // Uses the upstream PaddingDuration parameter, never fabricates padding.
    bool loss_recovery_without_padding = false;
  };

  struct googcc_queue_sample_t {
    std::uint64_t connection_epoch = 0;
    std::int64_t sampled_at_us = -1;
    std::uint64_t queued_ip_bytes = 0;
    bool accounting_valid = false;
    bool stopped = false;
  };

  struct googcc_probe_t {
    std::int32_t cluster_id = 0;
    std::int64_t target_kbps = 0;
    std::int64_t duration_us = 0;
    std::int64_t minimum_delta_us = 0;
    std::int32_t minimum_packets = 0;
    std::int64_t requested_at_us = -1;
  };

  struct googcc_snapshot_t {
    std::int64_t target_kbps = 0;  // Tracked video IP traffic; not encoder bitrate.
    std::int64_t pacing_kbps = 0;
    std::int64_t requested_padding_kbps = 0;
    std::int64_t padding_updated_at_us = -1;
    std::uint32_t network_loss_ppm = 0;
    std::int64_t network_rtt_us = -1;
    bool loss_recovery_without_padding = false;
    std::optional<std::int64_t> congestion_window_bytes;
    std::uint64_t feedback_batches = 0;
    std::uint64_t feedback_packet_changes = 0;
    std::uint64_t receiver_clock_resets = 0;
    std::uint64_t discarded_probe_requests = 0;
    std::uint64_t accepted_sends = 0;
    std::uint64_t rejected_send_events = 0;
    std::uint64_t rejected_feedback_events = 0;
    std::uint64_t unmapped_feedback_changes = 0;
    std::uint64_t sequence_mapping_evictions = 0;
    std::size_t sequence_mapping_size = 0;
    std::int64_t last_accepted_feedback_us = -1;  // Liveness, including empty changes.
    std::int64_t last_covered_feedback_us = -1;  // New mapped changes given to GoogCC.
    // New mapped coverage may arrive late. Use the actual send watermark as
    // well as processing liveness before allowing handoff, probes or upsteps.
    std::int64_t last_covered_send_us = -1;
    // Upstream ALR decision, not a local low-FPS or throughput heuristic.
    // Its timestamp is the last target update, not observation liveness.
    bool application_limited = false;
    std::int64_t target_updated_at_us = -1;
    double encoder_reduce_ratio = 0;
    std::optional<std::uint64_t> pacer_queue_ip_bytes;
    std::int64_t pacer_queue_at_us = -1;
    std::uint64_t accepted_queue_samples = 0;
    std::uint64_t rejected_queue_samples = 0;
    // Observed directly from pinned upstream RtcEventLog callbacks. A sent
    // cluster or a target change alone does not prove a valid probe estimate.
    std::uint64_t native_probe_successes = 0;
    std::uint64_t native_probe_failures = 0;
    std::int32_t last_native_probe_cluster = -1;
    std::int64_t last_native_probe_bps = 0;
    std::int64_t last_native_probe_at_us = -1;
    std::int32_t last_native_probe_failure = -1;
    std::uint64_t stale_probe_feedback_suppressed = 0;
    std::int64_t last_generated_probe_cluster = -1;
  };

  // Single serialized owner, consuming authoritative events before a later
  // timer/send/feedback time. Late OS submission events are rejected, never
  // retimestamped. This adapter provides estimates; it does not
  // apply encoder/FEC settings or transmit probes. The runtime owns handoff.
  class googcc_adapter_t {
  public:
    explicit googcc_adapter_t(const googcc_config_t &config);
    ~googcc_adapter_t();
    googcc_adapter_t(const googcc_adapter_t &) = delete;
    googcc_adapter_t &
    operator=(const googcc_adapter_t &) = delete;

    bool
    on_successful_send(const successful_send_event_t &event);
    bool
    on_feedback(const feedback_event_t &event);
    bool
    process_interval(std::int64_t now_us, std::optional<googcc_queue_sample_t> queue = {});
    std::vector<googcc_probe_t>
    take_probe_requests();
    const googcc_snapshot_t &
    snapshot() const noexcept;

  private:
    struct impl_t;
    std::unique_ptr<impl_t> impl_;
  };
}  // namespace transport
