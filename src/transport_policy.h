/**
 * @file src/transport_policy.h
 * @brief Immutable per-frame policy and truthful, session-local apply receipts.
 */
#pragma once

#include "transport_budget.h"

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace transport {
  enum class budget_basis_e { legacy,
    normalized };
  enum class policy_failure_e { none,
    unsupported,
    backend_failure,
    superseded,
    stopped };
  enum class control_source_e { legacy,
    manual,
    googcc,
    local };

  struct control_lease_t {
    std::uint64_t connection_epoch = 0;
    std::uint64_t control_epoch = 0;
    control_source_e source = control_source_e::legacy;
  };

  struct automatic_control_t {
    bool bitrate = true;
    bool fec = false;
    int maximum_total_kbps = 0;
    // Assigned by the arbiter, never supplied by the client. Survives automatic
    // revisions/handoff so the owner rebuilds once per explicit mode request.
    std::uint64_t activation_epoch = 0;
    bool
    operator==(const automatic_control_t &) const = default;
  };

  struct frame_policy_t {
    std::uint64_t connection_epoch = 0;
    std::uint64_t control_epoch = 1;
    std::uint64_t revision = 1;
    control_source_e control_source = control_source_e::legacy;
    budget_basis_e basis = budget_basis_e::legacy;
    budget_request_t budget;
    int encoder_kbps = 0;
    unsigned fec_base = 0;
    unsigned fec_key = 0;
    unsigned fec_recovery = 0;
    // Local encoder-production ceiling, independent of the network drain rate.
    // Only controller requests may set it; manual normalization clears it.
    std::optional<int> encoder_ceiling_kbps;
    std::optional<automatic_control_t> automatic_control;

    unsigned
    fec_for_frame(bool idr, bool recovery) const noexcept {
      return idr ? fec_key : recovery ? fec_recovery :
                                        fec_base;
    }
  };

  using frame_policy_ref_t = std::shared_ptr<const frame_policy_t>;

  struct policy_receipt_t {
    frame_policy_ref_t policy;
    bool encoder_applied = false;
    std::optional<std::uint64_t> first_sent_frame;
    policy_failure_e failure = policy_failure_e::none;
  };

  struct policy_snapshot_t {
    frame_policy_ref_t accepted;
    frame_policy_ref_t applied;
    bool encoder_initialized = false;
    bool stopped = false;
    std::vector<policy_receipt_t> receipts;
    bool experimental_packet_control_negotiated = false;
    bool experimental_video_pacer_enabled = false;
  };

  enum class policy_request_result_e { accepted,
    conflict,
    invalid,
    stopped };
  struct policy_request_result_t {
    policy_request_result_e result = policy_request_result_e::invalid;
    frame_policy_ref_t policy;
  };

  class policy_state_t {
  public:
    static constexpr std::size_t receipt_capacity = 32;
    explicit policy_state_t(frame_policy_t initial, int maximum_total_kbps,
      bool experimental_packet_control_negotiated = false, bool experimental_video_pacer_enabled = false);

    policy_request_result_t
    request_normalized(const budget_request_t &budget,
      unsigned fec_base, unsigned fec_key, unsigned fec_recovery,
      std::uint64_t expected_revision, std::uint64_t expected_control_epoch, std::string request_id = {});
    policy_request_result_t
    request_automatic_control(bool bitrate, bool fec, int maximum_total_kbps,
      std::uint64_t expected_revision, std::uint64_t expected_control_epoch, std::string request_id);
    policy_request_result_t
    request_legacy_change(std::optional<int> total_kbps, std::optional<unsigned> fec_percentage);
    // Explicitly negotiated startup, before granting a controller lease. Freeze
    // legacy ABR, normalize the existing ceiling, and wait for apply/send/feedback.
    policy_request_result_t
    prepare_normalized_controller(const control_lease_t &lease,
      const budget_request_t &budget, unsigned fec_base, unsigned fec_key, unsigned fec_recovery,
      std::uint64_t expected_revision);
    // Internal serialized handoff, never granted by a measurement request.
    // The caller must first pass its negotiated runtime gates. Even a same-
    // source replacement gets a fresh epoch, invalidating the old instance.
    policy_request_result_t
    transfer_control(const control_lease_t &lease,
      control_source_e target, std::uint64_t expected_revision);
    policy_request_result_t
    request_controller_update(const control_lease_t &lease,
      const budget_request_t &budget, unsigned fec_base, unsigned fec_key, unsigned fec_recovery,
      std::uint64_t expected_revision, std::string request_id = {}, std::optional<int> encoder_ceiling_kbps = {});

    frame_policy_ref_t
    active() const;
    frame_policy_ref_t
    begin_encoder_initialization();
    // Called by the one encoder owner; protects the applying revision from
    // history eviction while another producer submits a newer request.
    frame_policy_ref_t
    acquire_pending();
    bool
    acknowledge_encoder(const frame_policy_ref_t &policy, policy_failure_e failure);
    bool
    acknowledge_first_sent(const frame_policy_ref_t &policy, std::uint64_t frame);
    policy_snapshot_t
    snapshot() const;
    void
    stop();

  private:
    policy_request_result_t
    request_normalized_locked(const budget_request_t &budget,
      unsigned fec_base, unsigned fec_key, unsigned fec_recovery,
      std::uint64_t expected_revision, std::uint64_t expected_control_epoch,
      std::string request_id, control_source_e actor, bool manual_takeover, std::optional<int> encoder_ceiling_kbps = {},
      std::optional<automatic_control_t> automatic_control = {}, bool force_epoch = false);
    policy_request_result_t
    accept_locked(frame_policy_t policy);
    void
    trim_locked();
    const int maximum_total_kbps_;
    const bool experimental_packet_control_negotiated_;
    const bool experimental_video_pacer_enabled_;
    mutable std::mutex mutex_;
    frame_policy_ref_t accepted_;
    frame_policy_ref_t applied_;
    frame_policy_ref_t applying_;
    frame_policy_ref_t initializing_;
    bool encoder_initialized_ = false;
    bool stopped_ = false;
    std::map<std::uint64_t, policy_receipt_t> receipts_;
    struct request_entry_t {
      std::string id;
      budget_request_t budget;
      unsigned fec_base, fec_key, fec_recovery;
      std::uint64_t expected_revision, expected_control_epoch;
      control_source_e actor;
      frame_policy_ref_t policy;
      std::optional<int> encoder_ceiling_kbps;
      std::optional<automatic_control_t> automatic_control;
      bool force_epoch = false;
    };
    std::deque<request_entry_t> requests_;
  };

  // Encoder-owner history. Unknown/evicted output IDs must not use the latest
  // policy: returning null forces the caller to reject that output explicitly.
  class frame_policy_history_t {
  public:
    static constexpr std::size_t capacity = 256;
    void
    bind(std::uint64_t frame, frame_policy_ref_t policy);
    frame_policy_ref_t
    find(std::uint64_t frame) const;

  private:
    struct entry_t {
      std::uint64_t frame = 0;
      frame_policy_ref_t policy;
    };
    std::array<entry_t, capacity> entries_;
  };
}  // namespace transport
