/**
 * @file src/transport/transport_owner_inbox.h
 * @brief Bounded producer ingress and lifecycle commands for one transport owner.
 */
#pragma once

#include "transport_pacer.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace transport {
  class owner_inbox_t;

  class owner_flow_t {
  public:
    const std::uint64_t handle;
    const std::uint64_t connection_epoch;
    // A decoupled send context, never a strong session or broadcast_ref.
    const std::shared_ptr<void> context;

    bool is_closed() const noexcept;
    bool is_drained() const noexcept;
    void wait_drained() const;
    bool wait_drained_until(std::chrono::steady_clock::time_point deadline) const;

  private:
    friend class owner_inbox_t;
    owner_flow_t(std::uint64_t handle, std::uint64_t epoch, std::shared_ptr<void> context, std::shared_ptr<const void> cookie);
    void mark_drained() const;
    const std::shared_ptr<const void> cookie_;
    mutable std::atomic<bool> closed_ {false};
    mutable std::atomic<bool> drained_ {false};
    mutable std::mutex drain_mutex_;
    mutable std::condition_variable drain_cv_;
  };
  using owner_flow_ref_t = std::shared_ptr<const owner_flow_t>;

  struct owner_frame_t {
    owner_flow_ref_t flow;
    std::uint64_t frame_id = 0;
    frame_policy_ref_t policy;
    std::size_t owned_bytes = 0;
    std::optional<std::int64_t> deadline_origin_us;
    frame_dependency_e dependency = frame_dependency_e::reference;
    // Ownership of an encoded packet, or another immutable owned envelope.
    std::shared_ptr<void> payload;
  };

  struct owner_inbox_bounds_t {
    std::size_t maximum_flows = 16;
    std::size_t maximum_queued_frames = 32;
    std::size_t maximum_queued_bytes = 16U * 1024U * 1024U;
    std::size_t maximum_frames_per_flow = 16;
  };

  struct owner_drop_counts_t {
    std::uint64_t full_frames = 0;
    std::uint64_t closed_frames = 0;
    std::uint64_t invalid_frames = 0;
    std::uint64_t stale_frames = 0;
    std::uint64_t rejected_bytes = 0;
    std::uint64_t discarded_frames = 0;
    std::uint64_t discarded_bytes = 0;
    std::uint64_t dropped_feedback = 0;
    std::uint64_t dropped_feedback_bytes = 0;
    bool empty() const noexcept;
  };

  enum class owner_submit_result_e { accepted, full, closed, unknown_flow, invalid, stale };
  enum class owner_wait_result_e { woken, deadline, stopped };

  inline constexpr std::size_t max_owner_feedback_bytes = 884;
  inline constexpr std::size_t max_owner_feedback_per_flow = 8;
  inline constexpr std::size_t max_owner_feedback_messages = 128;
  inline constexpr std::size_t max_owner_feedback_queued_bytes = 113152;
  struct owner_feedback_t {
    owner_flow_ref_t flow;
    std::vector<std::uint8_t> authenticated_bytes;
    std::int64_t received_at_us = 0;
  };

  struct owner_command_t {
    owner_flow_ref_t flow;
    // Close wins over registration/limits/break. A close-before-registration is
    // still delivered and must be acknowledged after local ownership settles.
    bool close = false;
    bool register_flow = false;
    pacing_limits_t initial_limits;
    std::optional<pacing_limits_t> latest_limits;
    std::optional<std::uint64_t> highest_reference_break;
    owner_drop_counts_t drops;
    // Marker commands transfer every inbox-owned frame <= the marker. Closing
    // transfers all remaining frames. These are encoded-frame discards, never
    // UDP receipts; the caller explicitly settles/releases them.
    std::vector<owner_frame_t> discarded_frames;
  };

  struct owner_inbox_snapshot_t {
    std::size_t flows = 0;
    std::size_t queued_frames = 0;
    std::size_t queued_bytes = 0;
    std::size_t pending_commands = 0;
    std::size_t queued_feedback_messages = 0;
    std::size_t queued_feedback_bytes = 0;
    owner_drop_counts_t drops;
    bool stopped = false;
  };

  class owner_inbox_t {
  public:
    explicit owner_inbox_t(owner_inbox_bounds_t bounds = {});
    ~owner_inbox_t();
    owner_inbox_t(const owner_inbox_t &) = delete;
    owner_inbox_t &operator=(const owner_inbox_t &) = delete;

    // Identity is independent of the pacer's handle; the owner maps them.
    // Slots are not reusable until acknowledge_drained, IDs never reuse, and
    // callers supply a fresh cryptographic epoch for each connection.
    owner_flow_ref_t add_flow(std::uint64_t epoch, std::shared_ptr<void> context, const pacing_limits_t &initial_limits);
    owner_submit_result_e submit(owner_frame_t frame);
    // Already-authenticated control bytes are owned, not decoded here. Both
    // size and retained vector capacity are at most 884 bytes. Receipt time is
    // telemetry; the owner applies feedback at its processing time.
    owner_submit_result_e submit_feedback(const owner_flow_ref_t &flow, std::vector<std::uint8_t> authenticated_bytes, std::int64_t received_at_us);
    bool update_limits(const owner_flow_ref_t &flow, const pacing_limits_t &limits);
    bool mark_reference_break(const owner_flow_ref_t &flow, std::uint64_t frame_id);
    bool close(const owner_flow_ref_t &flow);
    void stop();

    // Serialized owner calls. One bounded command per active slot, closes first.
    // Call take_commands before take_frame: a pending marker/close fences that
    // flow until its discarded ownership has been transferred. Frames from
    // other flows can still progress. Returned frames must check is_closed()
    // immediately before every preparation/submission syscall.
    std::vector<owner_command_t> take_commands();
    std::optional<owner_frame_t> take_frame();
    std::optional<owner_feedback_t> take_feedback();
    // Call only AFTER pacer.stop_session and actual receipts / returned frames
    // and taken feedback have been settled. The inbox cannot inspect those external owners; this
    // call is their explicit barrier. It rejects an undelivered close or an
    // inbox with retained frames; repeated acknowledgement is idempotent.
    bool acknowledge_drained(const owner_flow_ref_t &flow);
    // Emergency cleanup ONLY after stop() and after the sole owner's taken
    // work, pacer queues and send mappings have been released. Does not allocate
    // commands/outcomes or claim UDP receipts. Releases retained ingress,
    // accounts encoded discards and acknowledges every sealed flow. Running
    // inboxes reject this call without changing any lifecycle or ownership.
    bool emergency_drained_after_owner_abort() noexcept;

    // Capture generation BEFORE processing commands/frames. Notification that
    // arrives between processing and wait is observed by the predicate. The
    // wait uses an absolute steady deadline, never a restarting relative delay.
    std::uint64_t wake_generation() const;
    owner_wait_result_e wait_until(std::uint64_t generation, std::chrono::steady_clock::time_point deadline);
    owner_inbox_snapshot_t snapshot() const;

  private:
    class impl_t;
    std::unique_ptr<impl_t> impl_;
  };
}  // namespace transport
