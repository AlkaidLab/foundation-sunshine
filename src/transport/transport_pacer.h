/**
 * @file src/transport/transport_pacer.h
 * @brief Owned, bounded deadline queues and successful-IP-byte pacing.
 */
#pragma once

#include "transport_feedback.h"
#include "transport_policy.h"

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace transport {
  // Rates are bytes/second at the IP layer. Credit is the integral of this
  // rate, capped by burst_bytes. Negative credit is explicit sending debt.
  struct pacing_bucket_limits_t {
    std::uint64_t rate_bytes_per_second = 0;
    std::uint64_t burst_bytes = 0;
    std::uint64_t maximum_debt_bytes = 0;
    bool
    operator==(const pacing_bucket_limits_t &) const = default;
  };

  struct pacing_limits_t {
    pacing_bucket_limits_t budget;
    pacing_bucket_limits_t instant;
    bool
    operator==(const pacing_limits_t &) const = default;
  };

  struct pacer_bounds_t {
    std::size_t maximum_sessions = 16;
    std::size_t maximum_frames_per_session = 32;
    std::size_t maximum_packets_per_frame = 4096;
    std::size_t maximum_queued_packets = 65536;
    std::size_t maximum_queued_payload_bytes = 16U * 1024U * 1024U;
    std::size_t maximum_batch_packets = 32;
    std::uint64_t batch_quantum_ip_bytes = 16U * 1024U;
    std::int64_t retry_delay_us = 1000;
    unsigned maximum_failed_attempts = 3;
  };

  enum class frame_dependency_e { reference,
    non_reference,
    recovery };

  // Move complete UDP payloads (encryption prefix included) into the queue.
  // The queue has sole ownership; no encoder, shard or stack buffers survive
  // through a borrow. metadata.ip_bytes must equal payload.size()+28/48.
  struct owned_paced_packet_t {
    std::vector<std::uint8_t> udp_payload;
    sent_packet_t metadata;
    bool ipv6 = false;
  };

  struct paced_frame_t {
    std::uint64_t frame_id = 0;
    frame_policy_ref_t policy;
    std::int64_t deadline_us = 0;
    frame_dependency_e dependency = frame_dependency_e::reference;
    std::vector<owned_paced_packet_t> packets;
  };

  enum class frame_send_result_e {
    complete,
    deadline_expired,
    cannot_meet_deadline,
    send_failed,
    queue_full,
    reference_chain_broken,
    stopped,
    sender_contract_broken
  };

  struct paced_frame_result_t {
    std::uint64_t session_handle = 0;
    std::uint64_t frame_id = 0;
    frame_policy_ref_t policy;
    frame_send_result_e result = frame_send_result_e::stopped;
    std::uint64_t submitted_packets = 0;
    std::uint64_t submitted_ip_bytes = 0;
    std::uint64_t abandoned_packets = 0;
    std::uint64_t abandoned_ip_bytes = 0;
    bool recovery_required = false;
    // All source-data datagrams were confirmed before the send deadline.
    // FEC may be incomplete; this does not prove receiver delivery or decode.
    // Unknown/invalid submission and explicit reference loss cannot set it.
    bool primary_complete = false;
  };

  enum class pacer_enqueue_result_e { queued,
    invalid,
    unknown_session,
    stopped,
    queue_full,
    clock_invalid };
  struct pacer_enqueue_result_t {
    pacer_enqueue_result_e result = pacer_enqueue_result_e::invalid;
    std::optional<paced_frame_result_t> dropped_frame;
  };

  enum class pacer_reference_break_result_e {
    marked,
    already_broken,
    stale,
    unknown_session,
    stopped,
    clock_invalid,
    resource_exhausted
  };
  struct pacer_reference_break_result_t {
    pacer_reference_break_result_e result = pacer_reference_break_result_e::unknown_session;
    // Only owned frames retired by this call. A frame dropped before enqueue
    // has no invented packet count, byte count, policy, or success receipt.
    std::vector<paced_frame_result_t> frames;
    bool recovery_required = false;
  };

  struct paced_packet_view_t {
    std::span<const std::uint8_t> udp_payload;
    sent_packet_t metadata;
    std::int64_t deadline_us = 0;
  };

  struct packet_submission_t {
    bool submitted = false;
    // Actual sender monotonic time after successful OS submission. A failed
    // submission has no timestamp. Successful entries must be nondecreasing.
    std::int64_t send_time_us = 0;
  };

  struct paced_batch_submission_t {
    std::vector<packet_submission_t> packets;
    std::int64_t completed_at_us = 0;
    bool failed_suffix_retryable = true;
    // A native result can confirm a prefix while leaving the suffix unknown.
    // Return that confirmed prefix, then close all owner accounting. Unknown
    // bytes must never be interpreted as failed packets eligible for retry.
    bool submission_known = true;
    // The unsubmitted suffix was not attempted because another real-time
    // outlet owns/exhausted the shared session budget. This is not OS failure.
    bool suffix_budget_deferred = false;
    // The adapter rechecked the probe lease immediately before its OS call.
    // No suffix was attempted; retry as ordinary media after cancelling tags.
    bool suffix_probe_cancelled = false;
  };

  struct paced_success_t {
    std::uint64_t session_handle = 0;
    frame_policy_ref_t policy;
    sent_packet_t packet;
    std::int64_t deadline_us = 0;
  };

  struct pacer_dispatch_result_t {
    bool attempted = false;
    bool clock_invalid = false;
    // Invalid or unknowable sender results close this owner rather than claim
    // that actual bytes or successful identities have been reconstructed.
    bool sender_contract_broken = false;
    bool accounting_closed = false;
    std::vector<paced_success_t> successful;
    std::vector<paced_frame_result_t> frames;
    std::optional<std::int64_t> next_wakeup_us;
  };

  struct probe_schedule_t {
    probe_info_t metadata;
    std::uint64_t minimum_group_ip_bytes = 0;
    std::int64_t next_send_us = -1;
    std::int64_t maximum_delay_us = 0;
  };
  // The optional SDK supplies probe timing/progress. The deadline queue owns
  // payloads, admission and actual success accounting; it has no probe algorithm.
  // Clones are for bounded admission forecasts and never receive OS receipts.
  class probe_scheduler_t {
  public:
    virtual ~probe_scheduler_t() = default;
    virtual void
    on_incoming_packet(std::uint32_t ip_bytes) = 0;
    virtual std::optional<probe_schedule_t>
    current(std::int64_t now_us) = 0;
    virtual bool
    on_group_sent(std::uint64_t ip_bytes, std::int64_t now_us) = 0;
    virtual std::unique_ptr<probe_scheduler_t>
    clone() const = 0;
  };
  enum class paced_probe_result_e {
    none,
    active,
    complete,
    invalid,
    busy,
    insufficient_media,
    deadline,
    cancelled,
    send_failed,
    budget_deferred,
    schedule_late
  };
  struct paced_probe_snapshot_t {
    std::int32_t cluster_id = -1;
    paced_probe_result_e result = paced_probe_result_e::none;
    std::uint64_t successful_packets = 0;
    std::uint64_t successful_ip_bytes = 0;
    std::uint32_t successful_groups = 0;
    std::int64_t next_send_us = -1;
  };

  struct pacer_snapshot_t {
    std::uint64_t submitted_packets = 0;
    std::uint64_t submitted_ip_bytes = 0;
    std::uint64_t externally_submitted_ip_bytes = 0;
    std::uint64_t budget_debt_bytes = 0;
    std::uint64_t instant_debt_bytes = 0;
    std::size_t queued_frames = 0;
    std::size_t queued_packets = 0;
    std::size_t queued_payload_bytes = 0;
    bool reference_chain_broken = false;
    bool stopped = false;
    bool accounting_valid = true;
    // Remaining datagrams at IP layer, excluding successful/abandoned packets.
    std::uint64_t queued_ip_bytes = 0;
    paced_probe_snapshot_t probe;
  };

  // Single serialized owner; callbacks must return promptly using a
  // nonblocking socket and must not call back into this object. No sleeps,
  // threads or wall-clock reads are hidden in the core. Every public time is
  // from the same injectable monotonic clock. dispatch performs at most one
  // bounded batch, then rotates to the next flow; retries do not block peers.
  // Returned packet views expire when the sender returns. The adapter must
  // check deadline_us before each fallback syscall; an OS call which itself
  // completes too late is still counted and reported as an expired frame.
  class deadline_pacer_t {
  public:
    using sender_t = std::function<paced_batch_submission_t(
      std::uint64_t session_handle, std::span<const paced_packet_view_t> packets)>;

    explicit deadline_pacer_t(pacer_bounds_t bounds = {});
    ~deadline_pacer_t();
    deadline_pacer_t(const deadline_pacer_t &) = delete;
    deadline_pacer_t &
    operator=(const deadline_pacer_t &) = delete;

    // Handles are never reused, including after stop_session. epoch is the
    // connection identity shared by policy, video wire identity and ledger.
    // The caller allocates a fresh epoch for each connection; the pacer does
    // not generate or retain unbounded history of cryptographic identities.
    std::optional<std::uint64_t>
    add_session(std::uint64_t connection_epoch,
      const pacing_limits_t &limits, std::int64_t now_us);
    bool
    update_limits(std::uint64_t handle, const pacing_limits_t &limits, std::int64_t now_us);
    bool
    set_host_limits(std::optional<pacing_limits_t> limits, std::int64_t now_us);
    pacer_enqueue_result_t
    enqueue_frame(std::uint64_t handle, paced_frame_t frame, std::int64_t now_us);
    // Report an externally lost encoded reference/recovery (ingress eviction,
    // construction failure) under this same serialized owner. The dropped frame
    // ID is consumed, including when it was never enqueued; packet sequence
    // reservation remains the adapter's responsibility. Retire its owned suffix
    // and later dependent references until a strictly newer recovery barrier.
    // A barrier only restores the chain after its complete, timely submission.
    // Older ordinary frames and non_reference frames remain eligible. Markers
    // at/before the last complete recovery are stale; repeated markers are
    // idempotent. A newer marker always extends the broken-frame watermark.
    // Rejected calls (including callback reentry and resource_exhausted) do not
    // mutate the clock/queues; resource exhaustion must be resolved before send.
    // No handle, epoch, reserved sequence, budget, or debt is reset.
    pacer_reference_break_result_t
    mark_reference_break(std::uint64_t handle, std::uint64_t frame_id, std::int64_t now_us);
    pacer_dispatch_result_t
    dispatch(std::int64_t now_us, const sender_t &sender);
    // Probe only a contiguous prefix of the already owned front frame. This
    // changes spacing/metadata, never payload, rate, credit, or wire identity.
    // One bounded cluster per flow; no padding or synthetic success records.
    // A group may span bounded batches, rotating between flows after each OS
    // submission. The whole group's actual cost must fit the existing buckets;
    // dispatch rechecks remaining funding and only counts completed groups.
    paced_probe_result_e
    start_probe(std::uint64_t handle, std::unique_ptr<probe_scheduler_t> &scheduler, std::int64_t now_us);
    bool
    cancel_probe(std::uint64_t handle, std::int64_t now_us);
    std::vector<paced_frame_result_t>
    stop_session(std::uint64_t handle, std::int64_t now_us);
    std::vector<paced_frame_result_t>
    stop(std::int64_t now_us);
    // Emergency owner cleanup after an exception: release every queued payload
    // without allocating outcomes or inventing UDP receipts. Preserves known
    // submissions, debt and identity watermarks, marks all accounting invalid,
    // and prevents any later send. Rejects sender callback reentry.
    bool
    abort_noexcept() noexcept;

    // Audio/control sent outside this queue must check allowance immediately
    // before nonblocking submission under the same owner. Debit always records
    // actual successful IP bytes, even for an unapproved send; exceeding the
    // allowance closes this owner's accounting instead of hiding the bytes.
    // On false, call stop() and consume its abandoned-frame results.
    bool
    external_allowance(std::uint64_t handle, std::uint64_t ip_bytes, std::int64_t now_us);
    bool
    debit_external_success(std::uint64_t handle, std::uint64_t ip_bytes, std::int64_t now_us);
    std::optional<pacer_snapshot_t>
    snapshot(std::uint64_t handle) const;
    pacer_snapshot_t
    host_snapshot() const;

  private:
    class impl_t;
    std::unique_ptr<impl_t> impl_;
  };
}  // namespace transport
