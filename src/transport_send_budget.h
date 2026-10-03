/**
 * @file src/transport_send_budget.h
 * @brief Shared session IP budget for serialized, nonblocking OS submissions.
 */
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

namespace transport {
  enum class send_traffic_e { video,
    audio,
    control,
    repair,
    probe,
    count };

  struct send_budget_limits_t {
    std::uint64_t rate_bytes_per_second = 0;
    std::uint64_t burst_bytes = 0;
    std::uint64_t maximum_debt_bytes = 0;
    bool
    operator==(const send_budget_limits_t &) const = default;
  };

  enum class send_budget_result_e { accepted,
    busy,
    insufficient,
    invalid,
    foreign_epoch,
    stale_revision,
    stopped };

  struct send_budget_event_t {
    std::uint64_t ordinal = 0;
    std::uint64_t connection_epoch = 0;
    std::int64_t at_us = 0;
    send_budget_limits_t limits;
    std::int64_t credit_microbytes = 0;
    bool accounting_valid = true;
    bool stopped = false;
    std::uint64_t policy_revision = 0;
  };

  struct send_budget_receipt_t: send_budget_event_t {
    send_traffic_e traffic = send_traffic_e::video;
    std::int64_t reserved_at_us = 0;
    std::uint64_t permitted_ip_bytes = 0;
    std::uint64_t successful_ip_bytes = 0;
    std::uint64_t successful_packets = 0;
    std::uint64_t uncertain_ip_bytes = 0;
    bool completion_known = false;
  };

  struct send_budget_snapshot_t: send_budget_event_t {
    std::array<std::uint64_t, static_cast<std::size_t>(send_traffic_e::count)> successful_ip_bytes {};
    std::array<std::uint64_t, static_cast<std::size_t>(send_traffic_e::count)> successful_packets {};
    std::uint64_t uncertain_ip_bytes = 0;
  };

  class session_send_budget_t {
    struct state_t;

  public:
    class permit_t {
    public:
      permit_t(const permit_t &) = delete;
      permit_t &
      operator=(const permit_t &) = delete;
      permit_t(permit_t &&other) noexcept;
      permit_t &
      operator=(permit_t &&) = delete;
      ~permit_t();

      std::uint64_t
      ip_bytes() const noexcept;
      bool
      allowed_to_send() const noexcept;
      bool
      begin_submission() noexcept;

      // Confirm only the known OS-success prefix. Unknown suffixes consume
      // conservative credit, remain explicitly uncertain, and close this epoch.
      // Like the held mutex, a permit must be settled on its reserving thread.
      send_budget_receipt_t
      complete(std::uint64_t successful_ip_bytes,
        std::uint64_t successful_packets, bool completion_known, std::int64_t completed_at_us) noexcept;
      send_budget_receipt_t
      cancel_before_send(std::int64_t now_us) noexcept;

    private:
      friend class session_send_budget_t;
      permit_t(std::shared_ptr<state_t> state, std::unique_lock<std::mutex> lock,
        send_traffic_e traffic, std::uint64_t bytes, std::int64_t at_us) noexcept;
      std::shared_ptr<state_t> state_;
      std::unique_lock<std::mutex> lock_;
      send_traffic_e traffic_;
      std::uint64_t bytes_;
      std::int64_t at_us_;
      bool active_ = true;
      bool started_ = false;
    };

    struct reservation_t {
      send_budget_result_e result = send_budget_result_e::invalid;
      std::optional<permit_t> permit;
    };
    struct update_t {
      send_budget_result_e result = send_budget_result_e::invalid;
      std::optional<send_budget_event_t> event;
    };

    session_send_budget_t(std::uint64_t connection_epoch, send_budget_limits_t limits, std::int64_t started_at_us,
      std::uint64_t policy_revision = 0);
    session_send_budget_t(const session_send_budget_t &) = delete;
    session_send_budget_t &
    operator=(const session_send_budget_t &) = delete;
    ~session_send_budget_t();

    // The permit holds one mutex across the bounded, nonblocking OS call.
    // No second sender or limit update can spend/refill credit while it is held.
    // A caller must either complete it or explicitly cancel before any send;
    // abandoning a live permit is an unknowable submission, never a refund.
    reservation_t
    try_reserve(std::uint64_t connection_epoch, send_traffic_e traffic,
      std::uint64_t maximum_ip_bytes, std::uint64_t minimum_ip_bytes, std::int64_t now_us);
    update_t
    try_update(std::uint64_t connection_epoch, send_budget_limits_t limits, std::int64_t now_us,
      std::uint64_t policy_revision = 0);
    std::optional<send_budget_snapshot_t>
    try_snapshot() const;
    // Closes admission immediately even when another thread owns a permit.
    // The already granted, known success prefix still has to be settled.
    bool
    stop(std::uint64_t connection_epoch) noexcept;

  private:
    std::shared_ptr<state_t> state_;
  };
}  // namespace transport
