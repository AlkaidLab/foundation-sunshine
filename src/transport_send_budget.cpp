#include "transport_send_budget.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <stdexcept>
#include <utility>

namespace transport {
  namespace {
    constexpr std::int64_t scale = 1000000;
    constexpr auto maximum_time_us = std::numeric_limits<std::int64_t>::max() / 4;
    constexpr std::uint64_t maximum_rate = 100000000;
    constexpr std::uint64_t maximum_credit_bytes = 1000000000;

    bool
    valid_limits(const send_budget_limits_t &limits) {
      return limits.rate_bytes_per_second > 0 && limits.rate_bytes_per_second <= maximum_rate &&
             limits.burst_bytes > 0 && limits.burst_bytes <= maximum_credit_bytes &&
             limits.maximum_debt_bytes <= maximum_credit_bytes - limits.burst_bytes;
    }

    bool
    increment(std::uint64_t &value, std::uint64_t delta) {
      if (delta > std::numeric_limits<std::uint64_t>::max() - value) return false;
      value += delta;
      return true;
    }
  }  // namespace

  struct session_send_budget_t::state_t {
    state_t(std::uint64_t epoch, send_budget_limits_t initial, std::int64_t now):
        connection_epoch(epoch), limits(initial), now_us(now), credit(initial.burst_bytes * scale) {}

    void
    advance(std::int64_t now) noexcept {
      // Concurrent callers may sample just before the previous owner releases
      // its mutex. Keep the effective clock; never award backwards-time credit.
      now = std::max(now, now_us);
      const auto ceiling = static_cast<std::int64_t>(limits.burst_bytes * scale);
      const auto needed = static_cast<std::uint64_t>(ceiling - credit);
      const auto elapsed = static_cast<std::uint64_t>(now - now_us);
      const auto rate = limits.rate_bytes_per_second;
      if (elapsed >= (needed + rate - 1) / rate)
        credit = ceiling;
      else
        credit += static_cast<std::int64_t>(elapsed * rate);
      now_us = now;
    }

    send_budget_event_t
    event() const noexcept {
      return { ordinal, connection_epoch, now_us, limits, credit, accounting_valid, stopped.load(std::memory_order_acquire), policy_revision };
    }

    void
    close_accounting() noexcept {
      accounting_valid = false;
      stopped.store(true, std::memory_order_release);
    }

    const std::uint64_t connection_epoch;
    mutable std::mutex mutex;
    std::atomic_bool stopped = false;
    std::atomic_bool permit_active = false;
    send_budget_limits_t limits;
    std::int64_t now_us;
    std::int64_t credit;
    std::uint64_t ordinal = 0;
    std::uint64_t policy_revision = 0;
    bool accounting_valid = true;
    std::array<std::uint64_t, static_cast<std::size_t>(send_traffic_e::count)> successful_ip_bytes {};
    std::array<std::uint64_t, static_cast<std::size_t>(send_traffic_e::count)> successful_packets {};
    std::uint64_t uncertain_ip_bytes = 0;
  };

  session_send_budget_t::session_send_budget_t(std::uint64_t epoch, send_budget_limits_t limits, std::int64_t now, std::uint64_t revision) {
    if (!epoch || !valid_limits(limits) || now < 0 || now > maximum_time_us)
      throw std::invalid_argument("Invalid shared session send budget");
    state_ = std::make_shared<state_t>(epoch, limits, now);
    state_->policy_revision = revision;
  }

  session_send_budget_t::~session_send_budget_t() {
    state_->stopped.store(true, std::memory_order_release);
  }

  session_send_budget_t::permit_t::permit_t(std::shared_ptr<state_t> state, std::unique_lock<std::mutex> lock,
    send_traffic_e traffic, std::uint64_t bytes, std::int64_t at_us) noexcept:
      state_(std::move(state)), lock_(std::move(lock)), traffic_(traffic), bytes_(bytes), at_us_(at_us) {}

  session_send_budget_t::permit_t::permit_t(permit_t &&other) noexcept:
      state_(std::move(other.state_)), lock_(std::move(other.lock_)), traffic_(other.traffic_), bytes_(other.bytes_),
      at_us_(other.at_us_), active_(std::exchange(other.active_, false)), started_(other.started_) {}

  session_send_budget_t::permit_t::~permit_t() {
    if (active_) (void) complete(0, 0, false, at_us_);
  }

  std::uint64_t
  session_send_budget_t::permit_t::ip_bytes() const noexcept {
    return active_ ? bytes_ : 0;
  }

  bool
  session_send_budget_t::permit_t::allowed_to_send() const noexcept {
    return active_ && !state_->stopped.load(std::memory_order_acquire);
  }

  bool
  session_send_budget_t::permit_t::begin_submission() noexcept {
    if (started_ || !allowed_to_send()) return false;
    started_ = true;
    return true;
  }

  send_budget_receipt_t
  session_send_budget_t::permit_t::complete(std::uint64_t successful_bytes,
    std::uint64_t packets, bool known, std::int64_t completed_at_us) noexcept {
    send_budget_receipt_t receipt;
    if (!active_) {
      receipt.accounting_valid = false;
      return receipt;
    }
    auto &state = *state_;
    const auto index = static_cast<std::size_t>(traffic_);
    const bool valid = (started_ || successful_bytes == 0) && successful_bytes <= bytes_ && ((successful_bytes == 0) == (packets == 0)) &&
                       packets <= successful_bytes / 28 && completed_at_us >= at_us_ && completed_at_us <= maximum_time_us;
    if (!valid) {
      successful_bytes = packets = 0;
      known = false;
      completed_at_us = state.now_us;
    }
    // Refill before charging at completion. Precharging at reservation then
    // refunding at completion would mint credit during a slow OS call.
    state.advance(completed_at_us);
    const auto uncertain = known ? 0 : bytes_ - successful_bytes;
    state.credit -= static_cast<std::int64_t>((successful_bytes + uncertain) * scale);
    const bool arithmetic_valid = increment(state.ordinal, 1) &&
                                  increment(state.successful_ip_bytes[index], successful_bytes) &&
                                  increment(state.successful_packets[index], packets) &&
                                  increment(state.uncertain_ip_bytes, uncertain);
    if (!known || !arithmetic_valid) state.close_accounting();
    static_cast<send_budget_event_t &>(receipt) = state.event();
    receipt.traffic = traffic_;
    receipt.reserved_at_us = at_us_;
    receipt.permitted_ip_bytes = bytes_;
    receipt.successful_ip_bytes = successful_bytes;
    receipt.successful_packets = packets;
    receipt.uncertain_ip_bytes = uncertain;
    receipt.completion_known = known;
    active_ = false;
    state.permit_active.store(false, std::memory_order_release);
    lock_.unlock();
    return receipt;
  }

  send_budget_receipt_t
  session_send_budget_t::permit_t::cancel_before_send(std::int64_t now) noexcept {
    return complete(0, 0, !started_, now);
  }

  session_send_budget_t::reservation_t
  session_send_budget_t::try_reserve(std::uint64_t epoch, send_traffic_e traffic,
    std::uint64_t maximum_bytes, std::uint64_t minimum_bytes, std::int64_t now) {
    if (epoch != state_->connection_epoch) return { send_budget_result_e::foreign_epoch, {} };
    if (static_cast<std::size_t>(traffic) >= static_cast<std::size_t>(send_traffic_e::count) ||
        minimum_bytes < 28 || maximum_bytes < minimum_bytes || maximum_bytes > maximum_credit_bytes ||
        now < 0 || now > maximum_time_us) return { send_budget_result_e::invalid, {} };
    if (state_->stopped.load(std::memory_order_acquire)) return { send_budget_result_e::stopped, {} };
    if (state_->permit_active.load(std::memory_order_acquire)) return { send_budget_result_e::busy, {} };
    std::unique_lock lock(state_->mutex, std::try_to_lock);
    if (!lock.owns_lock()) return { send_budget_result_e::busy, {} };
    if (state_->stopped.load(std::memory_order_acquire)) return { send_budget_result_e::stopped, {} };
    state_->advance(now);
    const auto available = std::max<std::int64_t>(0, state_->credit + static_cast<std::int64_t>(state_->limits.maximum_debt_bytes * scale)) / scale;
    const auto bytes = std::min(maximum_bytes, static_cast<std::uint64_t>(available));
    if (bytes < minimum_bytes) return { send_budget_result_e::insufficient, {} };
    state_->permit_active.store(true, std::memory_order_release);
    permit_t permit(state_, std::move(lock), traffic, bytes, state_->now_us);
    return { send_budget_result_e::accepted, std::move(permit) };
  }

  session_send_budget_t::update_t
  session_send_budget_t::try_update(std::uint64_t epoch, send_budget_limits_t limits, std::int64_t now, std::uint64_t revision) {
    if (epoch != state_->connection_epoch) return { send_budget_result_e::foreign_epoch, {} };
    if (!valid_limits(limits) || now < 0 || now > maximum_time_us) return { send_budget_result_e::invalid, {} };
    if (state_->stopped.load(std::memory_order_acquire)) return { send_budget_result_e::stopped, {} };
    if (state_->permit_active.load(std::memory_order_acquire)) return { send_budget_result_e::busy, {} };
    std::unique_lock lock(state_->mutex, std::try_to_lock);
    if (!lock.owns_lock()) return { send_budget_result_e::busy, {} };
    if (state_->stopped.load(std::memory_order_acquire)) return { send_budget_result_e::stopped, {} };
    if (revision < state_->policy_revision) return { send_budget_result_e::stale_revision, {} };
    if (revision != 0 && revision == state_->policy_revision && limits != state_->limits)
      return { send_budget_result_e::invalid, {} };
    state_->advance(now);
    if (limits == state_->limits && revision == state_->policy_revision) return { send_budget_result_e::accepted, {} };
    state_->policy_revision = revision;
    state_->limits = limits;
    state_->credit = std::min(state_->credit, static_cast<std::int64_t>(limits.burst_bytes * scale));
    if (!increment(state_->ordinal, 1)) state_->close_accounting();
    return { send_budget_result_e::accepted, state_->event() };
  }

  std::optional<send_budget_snapshot_t>
  session_send_budget_t::try_snapshot() const {
    if (state_->permit_active.load(std::memory_order_acquire)) return {};
    std::unique_lock lock(state_->mutex, std::try_to_lock);
    if (!lock.owns_lock()) return {};
    send_budget_snapshot_t snapshot;
    static_cast<send_budget_event_t &>(snapshot) = state_->event();
    snapshot.successful_ip_bytes = state_->successful_ip_bytes;
    snapshot.successful_packets = state_->successful_packets;
    snapshot.uncertain_ip_bytes = state_->uncertain_ip_bytes;
    return snapshot;
  }

  bool
  session_send_budget_t::stop(std::uint64_t epoch) noexcept {
    if (epoch != state_->connection_epoch) return false;
    state_->stopped.store(true, std::memory_order_release);
    return true;
  }
}  // namespace transport
