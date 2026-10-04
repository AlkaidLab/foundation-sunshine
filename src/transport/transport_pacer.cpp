#include "transport_pacer.h"

#include "transport_budget.h"
#include "transport_credit.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace transport {
  namespace {
    constexpr auto scale = detail::credit_scale;
    constexpr std::uint64_t maximum_credit_bytes = 1ULL << 32;
    constexpr std::uint64_t maximum_rate = 1000000000000ULL;
    // Experimental scheduling tolerance; not a V6-approved production value.
    constexpr std::uint64_t probe_schedule_slack_us = 5000;

    bool
    valid(const pacing_limits_t &limits) {
      for (const auto &bucket : { limits.budget, limits.instant }) {
        if (bucket.rate_bytes_per_second > maximum_rate || bucket.burst_bytes > maximum_credit_bytes ||
            bucket.maximum_debt_bytes > maximum_credit_bytes) return false;
      }
      return true;
    }

    std::int64_t
    add_time(std::int64_t now, std::uint64_t delta) {
      const auto room = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() - now);
      return delta > room ? std::numeric_limits<std::int64_t>::max() : now + static_cast<std::int64_t>(delta);
    }

    std::uint64_t
    ceil_div(std::uint64_t n, std::uint64_t d) {
      return n / d + (n % d != 0);
    }

    struct bucket_t {
      pacing_bucket_limits_t limits;
      std::int64_t credit = 0;
      std::int64_t updated_at_us = 0;

      void
      initialize(const pacing_bucket_limits_t &new_limits, std::int64_t now) {
        limits = new_limits;
        credit = static_cast<std::int64_t>(limits.burst_bytes) * scale;
        updated_at_us = now;
      }

      void
      advance(std::int64_t now) {
        const auto elapsed = static_cast<std::uint64_t>(now - updated_at_us);
        credit = detail::refill_credit(credit, limits.rate_bytes_per_second, limits.burst_bytes, elapsed);
        updated_at_us = now;
      }

      void
      update(const pacing_bucket_limits_t &new_limits, std::int64_t now) {
        advance(now);  // Old rate is integrated only up to its change boundary.
        limits = new_limits;
        credit = std::min(credit, static_cast<std::int64_t>(limits.burst_bytes) * scale);
        // Negative credit is never forgiven by a rate/debt-limit reduction.
      }

      std::uint64_t
      allowance() const {
        const auto available = credit + static_cast<std::int64_t>(limits.maximum_debt_bytes) * scale;
        return available <= 0 ? 0 : static_cast<std::uint64_t>(available / scale);
      }

      std::uint64_t
      debt() const {
        return credit >= 0 ? 0 : ceil_div(static_cast<std::uint64_t>(-credit), scale);
      }

      std::int64_t
      earliest(std::uint64_t bytes, std::int64_t now) const {
        const auto required = static_cast<std::int64_t>(bytes * scale);
        const auto available = credit + static_cast<std::int64_t>(limits.maximum_debt_bytes) * scale;
        if (required <= available) return now;
        if (limits.rate_bytes_per_second == 0) return std::numeric_limits<std::int64_t>::max();
        return add_time(now, ceil_div(static_cast<std::uint64_t>(required - available), limits.rate_bytes_per_second));
      }

      bool
      packet_fits(std::uint64_t bytes) const {
        return bytes <= limits.burst_bytes + limits.maximum_debt_bytes;
      }

      bool
      debit(std::uint64_t bytes) {
        const bool allowed = bytes <= allowance();
        // All queued data is bounded by 128 MiB. External debit uses a hard
        // 4 GiB guard. Unexpected excess is accounted, then that flow closes.
        credit -= static_cast<std::int64_t>(bytes * scale);
        return allowed;
      }
    };

    struct pair_t {
      bucket_t budget, instant;
      void
      initialize(const pacing_limits_t &limits, std::int64_t now) {
        budget.initialize(limits.budget, now);
        instant.initialize(limits.instant, now);
      }
      void
      advance(std::int64_t now) {
        budget.advance(now);
        instant.advance(now);
      }
      void
      update(const pacing_limits_t &limits, std::int64_t now) {
        budget.update(limits.budget, now);
        instant.update(limits.instant, now);
      }
      std::uint64_t
      allowance() const { return std::min(budget.allowance(), instant.allowance()); }
      std::int64_t
      earliest(std::uint64_t bytes, std::int64_t now) const {
        return std::max(budget.earliest(bytes, now), instant.earliest(bytes, now));
      }
      bool
      packet_fits(std::uint64_t bytes) const { return budget.packet_fits(bytes) && instant.packet_fits(bytes); }
      bool
      debit(std::uint64_t bytes) {
        const auto a = budget.debit(bytes);
        const auto b = instant.debit(bytes);
        return a && b;
      }
    };

    bool
    increment(std::uint64_t &counter, std::uint64_t n) {
      if (n > std::numeric_limits<std::uint64_t>::max() - counter) return false;
      counter += n;
      return true;
    }
  }  // namespace

  class deadline_pacer_t::impl_t {
  public:
    struct frame_t {
      paced_frame_t frame;
      std::vector<std::uint32_t> suffix_maximum_ip_bytes;
      std::size_t cursor = 0;
      std::uint64_t remaining_ip_bytes = 0;
      std::uint64_t submitted_ip_bytes = 0;
      std::uint64_t submitted_packets = 0;
      unsigned failed_attempts = 0;
      std::uint64_t total_data_packets = 0;
      std::uint64_t submitted_data_packets = 0;
      bool late_data = false;
      std::uint64_t trailing_fec_ip_bytes = 0;
    };
    struct session_t {
      std::uint64_t epoch = 0;
      pair_t buckets;
      std::deque<frame_t> frames;
      struct probe_t {
        probe_info_t metadata;
        std::uint64_t frame_id = 0;
        std::uint64_t group_bytes = 0;
        std::uint64_t last_sequence = 0;
        std::int64_t delta_us = 0;
        std::uint64_t group_sent_bytes = 0;
      };
      std::optional<probe_t> probe;
      std::int32_t last_probe_cluster = -1;
      std::optional<std::uint64_t> last_frame;
      std::optional<std::uint64_t> last_reserved_sequence;
      // A recovery older than a known, subsequently dropped encoded reference
      // cannot repair that later break. Intermediate recoveries conservatively
      // retain the break until a recovery newer than all known breaks completes.
      std::optional<std::uint64_t> first_broken_frame, last_broken_frame;
      std::optional<std::uint64_t> last_complete_recovery;
      std::int64_t retry_at_us = 0;
      pacer_snapshot_t stats;
    };

    explicit impl_t(pacer_bounds_t new_bounds): bounds(new_bounds) {
      if (bounds.maximum_sessions == 0 || bounds.maximum_sessions > 256 ||
          bounds.maximum_frames_per_session == 0 || bounds.maximum_frames_per_session > 256 ||
          bounds.maximum_packets_per_frame == 0 || bounds.maximum_packets_per_frame > 16384 ||
          bounds.maximum_queued_packets == 0 || bounds.maximum_queued_packets > (1U << 20) ||
          bounds.maximum_queued_payload_bytes == 0 || bounds.maximum_queued_payload_bytes > 128U * 1024U * 1024U ||
          bounds.maximum_batch_packets == 0 || bounds.maximum_batch_packets > 256 ||
          bounds.batch_quantum_ip_bytes == 0 || bounds.batch_quantum_ip_bytes > maximum_credit_bytes ||
          bounds.retry_delay_us <= 0 || bounds.maximum_failed_attempts == 0 || bounds.maximum_failed_attempts > 1024)
        throw std::invalid_argument("invalid deadline pacer bounds");
      sessions.reserve(bounds.maximum_sessions);
      order.reserve(bounds.maximum_sessions);
    }

    bool
    accept_time(std::int64_t now) {
      if (busy || now < 0 || now < clock_us) return false;
      clock_us = now;
      if (host) host->advance(now);
      return true;
    }
    void
    advance_session(session_t &session, std::int64_t now) { session.buckets.advance(now); }
    void
    end_probe(session_t &session, paced_probe_result_e reason) {
      if (!session.probe) return;
      session.probe.reset();
      session.stats.probe.result = reason;
      session.stats.probe.next_send_us = -1;
    }
    void
    break_chain(session_t &session, std::uint64_t frame) {
      session.first_broken_frame = session.first_broken_frame ? std::min(*session.first_broken_frame, frame) : frame;
      session.last_broken_frame = session.last_broken_frame ? std::max(*session.last_broken_frame, frame) : frame;
      session.stats.reference_chain_broken = true;
    }
    paced_frame_result_t
    finish(std::uint64_t handle, session_t &session, frame_send_result_e reason, std::size_t index = 0) {
      auto &queued = session.frames[index];
      const auto &frame = queued.frame;
      if (session.probe && session.probe->frame_id == frame.frame_id)
        end_probe(session, paced_probe_result_e::cancelled);
      const bool valid_source_result = reason == frame_send_result_e::complete || reason == frame_send_result_e::deadline_expired ||
                                       reason == frame_send_result_e::cannot_meet_deadline || reason == frame_send_result_e::send_failed;
      const bool primary_complete = valid_source_result && queued.total_data_packets != 0 &&
                                    queued.submitted_data_packets == queued.total_data_packets && !queued.late_data;
      // Once all primary data is timely and known, abandoning optional FEC
      // cannot damage an otherwise submitted reference. A recovery still must
      // be newer than every known break; an unknown native result never clears.
      if (primary_complete && frame.dependency == frame_dependency_e::recovery) {
        session.last_complete_recovery = frame.frame_id;
        if (!session.last_broken_frame || frame.frame_id > *session.last_broken_frame) {
          session.first_broken_frame.reset();
          session.last_broken_frame.reset();
        }
      }
      else if (!primary_complete && reason != frame_send_result_e::reference_chain_broken &&
               frame.dependency != frame_dependency_e::non_reference)
        break_chain(session, frame.frame_id);
      session.stats.reference_chain_broken = session.first_broken_frame.has_value();
      paced_frame_result_t result { handle, frame.frame_id, frame.policy, reason, queued.submitted_packets,
        queued.submitted_ip_bytes, static_cast<std::uint64_t>(frame.packets.size() - queued.cursor),
        queued.remaining_ip_bytes, session.stats.reference_chain_broken, primary_complete };
      for (std::size_t i = queued.cursor; i < frame.packets.size(); ++i) {
        const auto size = frame.packets[i].udp_payload.size();
        payload_bytes -= size;
        session.stats.queued_payload_bytes -= size;
        if (!frame.packets[i].udp_payload.empty()) {
          session.stats.queued_ip_bytes -= frame.packets[i].metadata.ip_bytes;
          --queued_packets;
          --session.stats.queued_packets;
        }
      }
      if (index == 0)
        session.frames.pop_front();
      else
        session.frames.erase(session.frames.begin() + static_cast<std::ptrdiff_t>(index));
      --session.stats.queued_frames;
      return result;
    }
    std::uint64_t
    allowance(const session_t &session) const {
      return host ? std::min(host->allowance(), session.buckets.allowance()) : session.buckets.allowance();
    }
    std::int64_t
    earliest(const session_t &session, std::uint64_t bytes, std::int64_t now) const {
      return host ? std::max(host->earliest(bytes, now), session.buckets.earliest(bytes, now)) : session.buckets.earliest(bytes, now);
    }
    bool
    packet_fits(const session_t &session, std::uint64_t bytes) const {
      return session.buckets.packet_fits(bytes) && (!host || host->packet_fits(bytes));
    }
    bool
    debit(session_t &session, std::uint64_t bytes) {
      const bool a = session.buckets.debit(bytes);
      const bool b = !host || host->debit(bytes);
      return a && b;
    }
    void
    note_wakeup(std::optional<std::int64_t> &target, std::int64_t value) const {
      if (value != std::numeric_limits<std::int64_t>::max()) target = target ? std::min(*target, value) : value;
    }
    void
    close_accounting(std::optional<std::uint64_t> contract_session, pacer_dispatch_result_t &result) {
      stopped = true;
      host_stats.accounting_valid = false;
      result.accounting_closed = true;
      result.sender_contract_broken = contract_session.has_value();
      for (const auto handle : order) {
        auto &session = sessions.at(handle);
        session.stats.stopped = true;
        if (contract_session && handle == *contract_session) session.stats.accounting_valid = false;
        while (!session.frames.empty()) result.frames.push_back(finish(handle, session,
          contract_session && handle == *contract_session ? frame_send_result_e::sender_contract_broken : frame_send_result_e::stopped));
      }
      result.next_wakeup_us.reset();
    }

    pacer_bounds_t bounds;
    std::unordered_map<std::uint64_t, session_t> sessions;
    std::vector<std::uint64_t> order;
    std::size_t cursor = 0;
    std::optional<pair_t> host;
    pacer_snapshot_t host_stats;
    std::uint64_t next_handle = 1;
    std::size_t payload_bytes = 0;
    std::size_t queued_packets = 0;
    std::int64_t clock_us = 0;
    bool stopped = false;
    bool busy = false;
  };

  deadline_pacer_t::deadline_pacer_t(pacer_bounds_t bounds): impl_(std::make_unique<impl_t>(bounds)) {}
  deadline_pacer_t::~deadline_pacer_t() = default;

  std::optional<std::uint64_t>
  deadline_pacer_t::add_session(std::uint64_t epoch, const pacing_limits_t &limits, std::int64_t now) {
    auto &p = *impl_;
    if (epoch == 0 || !valid(limits) || p.stopped || !p.accept_time(now) ||
        p.sessions.size() >= p.bounds.maximum_sessions || p.next_handle == std::numeric_limits<std::uint64_t>::max()) return {};
    for (const auto &[handle, session] : p.sessions) {
      (void) handle;
      if (session.epoch == epoch) return {};
    }
    impl_t::session_t session;
    session.epoch = epoch;
    session.buckets.initialize(limits, now);
    const auto handle = p.next_handle++;
    p.sessions.emplace(handle, std::move(session));
    p.order.push_back(handle);
    return handle;
  }

  bool
  deadline_pacer_t::update_limits(std::uint64_t handle, const pacing_limits_t &limits, std::int64_t now) {
    auto &p = *impl_;
    auto it = p.sessions.find(handle);
    if (!valid(limits) || p.stopped || it == p.sessions.end() || it->second.stats.stopped || !p.accept_time(now)) return false;
    it->second.buckets.update(limits, now);
    return true;
  }

  bool
  deadline_pacer_t::set_host_limits(std::optional<pacing_limits_t> limits, std::int64_t now) {
    auto &p = *impl_;
    if ((limits && !valid(*limits)) || p.stopped || !p.accept_time(now)) return false;
    if (limits) {
      if (p.host)
        p.host->update(*limits, now);
      else {
        p.host.emplace();
        p.host->initialize(*limits, now);
      }
    }
    else {
      // Removing a host cap while it has real debt must not mint new credit.
      if (p.host && (p.host->budget.debt() != 0 || p.host->instant.debt() != 0)) return false;
      p.host.reset();
    }
    return true;
  }

  pacer_enqueue_result_t
  deadline_pacer_t::enqueue_frame(std::uint64_t handle, paced_frame_t frame, std::int64_t now) {
    auto &p = *impl_;
    const auto it = p.sessions.find(handle);
    if (p.stopped) return { pacer_enqueue_result_e::stopped, {} };
    if (it == p.sessions.end()) return { pacer_enqueue_result_e::unknown_session, {} };
    auto &session = it->second;
    if (session.stats.stopped) return { pacer_enqueue_result_e::stopped, {} };
    if (!p.accept_time(now)) return { pacer_enqueue_result_e::clock_invalid, {} };
    if (!frame.policy || frame.policy->connection_epoch != session.epoch || frame.deadline_us < 0 ||
        frame.packets.empty() || frame.packets.size() > p.bounds.maximum_packets_per_frame ||
        (frame.dependency != frame_dependency_e::reference && frame.dependency != frame_dependency_e::non_reference &&
          frame.dependency != frame_dependency_e::recovery) ||
        (session.last_frame && frame.frame_id <= *session.last_frame)) return {};
    std::size_t bytes = 0;
    std::uint64_t ip_bytes = 0;
    std::uint64_t data_packets = 0;
    auto last_sequence = session.last_reserved_sequence;
    for (const auto &packet : frame.packets) {
      const auto computed = ip_datagram_bytes(packet.udp_payload.size(), packet.ipv6);
      if (packet.udp_payload.empty() || !computed || *computed > 65535 || *computed != packet.metadata.ip_bytes ||
          packet.metadata.extended_sequence == std::numeric_limits<std::uint64_t>::max() ||
          (last_sequence && packet.metadata.extended_sequence <= *last_sequence) ||
          packet.metadata.frame_id != frame.frame_id || packet.metadata.policy_revision != frame.policy->revision) return {};
      if (packet.metadata.kind != packet_kind_e::data && packet.metadata.kind != packet_kind_e::fec &&
          packet.metadata.kind != packet_kind_e::repair && packet.metadata.kind != packet_kind_e::probe) return {};
      if (frame.dependency != frame_dependency_e::non_reference && packet.metadata.kind != packet_kind_e::data &&
          packet.metadata.kind != packet_kind_e::fec) return {};
      if (packet.metadata.kind == packet_kind_e::data) ++data_packets;
      last_sequence = packet.metadata.extended_sequence;
      bytes += packet.udp_payload.size();
      ip_bytes += *computed;
    }
    if (frame.dependency != frame_dependency_e::non_reference && data_packets == 0) return {};
    // Even a refused encoded frame consumes its already reserved identities.
    session.last_frame = frame.frame_id;
    session.last_reserved_sequence = last_sequence;
    if (session.frames.size() >= p.bounds.maximum_frames_per_session ||
        frame.packets.size() > p.bounds.maximum_queued_packets - p.queued_packets ||
        bytes > p.bounds.maximum_queued_payload_bytes - p.payload_bytes) {
      if (frame.dependency != frame_dependency_e::non_reference) p.break_chain(session, frame.frame_id);
      return { pacer_enqueue_result_e::queue_full, paced_frame_result_t { handle, frame.frame_id, frame.policy,
                                                     frame_send_result_e::queue_full, 0, 0, static_cast<std::uint64_t>(frame.packets.size()), ip_bytes,
                                                     session.stats.reference_chain_broken } };
    }
    const auto reserved_frame = frame.frame_id;
    const auto reserved_policy = frame.policy;
    const auto packet_count = frame.packets.size();
    const auto dependency = frame.dependency;
    try {
      std::vector<std::uint32_t> suffix_maximum(packet_count);
      std::uint32_t maximum = 0;
      std::uint64_t trailing_fec_ip_bytes = 0;
      bool primary_suffix = frame.dependency == frame_dependency_e::non_reference;
      for (std::size_t i = packet_count; i > 0; --i) {
        if (!primary_suffix) {
          if (frame.packets[i - 1].metadata.kind == packet_kind_e::data) {
            primary_suffix = true;
            maximum = 0;
          }
          else
            trailing_fec_ip_bytes += frame.packets[i - 1].metadata.ip_bytes;
        }
        maximum = std::max(maximum, frame.packets[i - 1].metadata.ip_bytes);
        suffix_maximum[i - 1] = maximum;
      }
      session.frames.push_back({ std::move(frame), std::move(suffix_maximum), 0, ip_bytes, 0, 0, 0, data_packets, 0, false, trailing_fec_ip_bytes });
    }
    catch (const std::bad_alloc &) {
      if (dependency != frame_dependency_e::non_reference) p.break_chain(session, reserved_frame);
      return { pacer_enqueue_result_e::queue_full, paced_frame_result_t { handle, reserved_frame, reserved_policy,
                                                     frame_send_result_e::queue_full, 0, 0, static_cast<std::uint64_t>(packet_count), ip_bytes,
                                                     session.stats.reference_chain_broken } };
    }
    p.payload_bytes += bytes;
    p.queued_packets += packet_count;
    session.stats.queued_payload_bytes += bytes;
    session.stats.queued_ip_bytes += ip_bytes;
    session.stats.queued_packets += packet_count;
    ++session.stats.queued_frames;
    return { pacer_enqueue_result_e::queued, {} };
  }

  pacer_reference_break_result_t
  deadline_pacer_t::mark_reference_break(std::uint64_t handle, std::uint64_t frame_id, std::int64_t now) {
    auto &p = *impl_;
    pacer_reference_break_result_t result;
    if (p.stopped) {
      result.result = pacer_reference_break_result_e::stopped;
      return result;
    }
    const auto it = p.sessions.find(handle);
    if (it == p.sessions.end()) return result;
    auto &session = it->second;
    result.recovery_required = session.stats.reference_chain_broken;
    if (session.stats.stopped) {
      result.result = pacer_reference_break_result_e::stopped;
      return result;
    }
    if (p.busy || now < 0 || now < p.clock_us) {
      result.result = pacer_reference_break_result_e::clock_invalid;
      return result;
    }
    if (session.last_complete_recovery && frame_id <= *session.last_complete_recovery) {
      result.result = pacer_reference_break_result_e::stale;
      return result;
    }
    // Allocate all possible retirement results before changing time, debt or
    // chain state. Every operation after this reserve moves existing owners.
    try {
      result.frames.reserve(session.frames.size());
    }
    catch (const std::bad_alloc &) {
      result.result = pacer_reference_break_result_e::resource_exhausted;
      return result;
    }
    (void) p.accept_time(now);
    p.advance_session(session, now);
    const bool changed = !session.first_broken_frame || frame_id < *session.first_broken_frame || frame_id > *session.last_broken_frame;
    p.break_chain(session, frame_id);
    session.last_frame = session.last_frame ? std::max(*session.last_frame, frame_id) : frame_id;
    bool recovery_barrier = false;
    for (std::size_t i = 0; i < session.frames.size();) {
      const auto &frame = session.frames[i].frame;
      if (frame.frame_id == frame_id) {
        // finish reports existing successes, destroys only remaining owned
        // payloads, and never refunds an already submitted datagram's debt.
        result.frames.push_back(p.finish(handle, session, frame_send_result_e::reference_chain_broken, i));
      }
      else if (frame.dependency == frame_dependency_e::recovery && frame.frame_id > *session.last_broken_frame) {
        if (frame.deadline_us <= now) {
          result.frames.push_back(p.finish(handle, session, frame_send_result_e::deadline_expired, i));
          recovery_barrier = false;
        }
        else {
          recovery_barrier = true;
          ++i;
        }
      }
      else if (frame.dependency == frame_dependency_e::reference && frame.frame_id > *session.first_broken_frame && !recovery_barrier) {
        result.frames.push_back(p.finish(handle, session, frame_send_result_e::reference_chain_broken, i));
      }
      else
        ++i;
    }
    result.recovery_required = true;
    result.result = changed || !result.frames.empty() ? pacer_reference_break_result_e::marked : pacer_reference_break_result_e::already_broken;
    return result;
  }

  pacer_dispatch_result_t
  deadline_pacer_t::dispatch(std::int64_t now, const sender_t &sender) {
    auto &p = *impl_;
    pacer_dispatch_result_t result;
    if (!p.accept_time(now)) {
      result.clock_invalid = true;
      return result;
    }
    if (p.stopped || p.order.empty() || !sender) return result;
    const auto count = p.order.size();
    std::size_t queued_frames = 0;
    for (const auto handle : p.order) queued_frames += p.sessions.at(handle).frames.size();
    result.frames.reserve(queued_frames);
    // Deadline changes can make a later encoded frame expire before the
    // front of its flow. Retire it without discarding valid earlier frames.
    for (const auto handle : p.order) {
      auto &session = p.sessions.at(handle);
      for (std::size_t i = 0; i < session.frames.size();) {
        if (session.frames[i].frame.deadline_us <= now)
          result.frames.push_back(p.finish(handle, session, frame_send_result_e::deadline_expired, i));
        else {
          p.note_wakeup(result.next_wakeup_us, session.frames[i].frame.deadline_us);
          ++i;
        }
      }
    }
    for (std::size_t visited = 0; visited < count; ++visited) {
      const auto index = (p.cursor + visited) % count;
      const auto handle = p.order[index];
      auto &session = p.sessions.at(handle);
      if (session.stats.stopped) continue;
      p.advance_session(session, now);
      while (!session.frames.empty()) {
        const auto &queued = session.frames.front();
        const auto &frame = queued.frame;
        // Predict completion through the last source packet. Parity between
        // source blocks still costs budget; only the final FEC tail is optional.
        const auto required_ip_bytes = queued.remaining_ip_bytes -
                                       (queued.submitted_data_packets < queued.total_data_packets ? queued.trailing_fec_ip_bytes : 0);
        std::optional<frame_send_result_e> reason;
        if (now >= frame.deadline_us)
          reason = frame_send_result_e::deadline_expired;
        else if (frame.dependency == frame_dependency_e::reference && session.first_broken_frame &&
                 frame.frame_id > *session.first_broken_frame)
          reason = frame_send_result_e::reference_chain_broken;
        else if (p.earliest(session, required_ip_bytes, now) >= frame.deadline_us ||
                 !p.packet_fits(session, queued.suffix_maximum_ip_bytes[queued.cursor]))
          reason = frame_send_result_e::cannot_meet_deadline;
        if (!reason) break;
        result.frames.push_back(p.finish(handle, session, *reason));
      }
      if (session.frames.empty()) continue;
      auto &queued = session.frames.front();
      auto &frame = queued.frame;
      p.note_wakeup(result.next_wakeup_us, frame.deadline_us);
      if (session.probe) {
        if (now > add_time(session.stats.probe.next_send_us, probe_schedule_slack_us))
          p.end_probe(session, paced_probe_result_e::schedule_late);
        else if (now < session.stats.probe.next_send_us) {
          p.note_wakeup(result.next_wakeup_us, session.stats.probe.next_send_us);
          continue;
        }
      }
      if (session.retry_at_us > now) {
        p.note_wakeup(result.next_wakeup_us, session.retry_at_us);
        continue;
      }
      const auto first_bytes = frame.packets[queued.cursor].metadata.ip_bytes;
      if (session.buckets.allowance() < first_bytes) {
        p.note_wakeup(result.next_wakeup_us, session.buckets.earliest(first_bytes, now));
        continue;
      }
      if (p.host && p.host->allowance() < first_bytes) {
        // Keep this eligible flow's turn until the shared host budget can
        // fund one packet. Otherwise an endless stream of small packets could
        // steal every refill and starve a larger, otherwise ready flow.
        p.cursor = index;
        p.note_wakeup(result.next_wakeup_us, p.host->earliest(first_bytes, now));
        return result;
      }
      std::vector<paced_packet_view_t> views;
      views.reserve(p.bounds.maximum_batch_packets);
      std::uint64_t scheduled_bytes = 0;
      const auto allowance = p.allowance(session);
      if (session.probe) {
        std::uint64_t group_cost = 0;
        for (auto i = queued.cursor; i < frame.packets.size(); ++i) {
          group_cost += frame.packets[i].metadata.ip_bytes;
          if (session.probe->group_sent_bytes + group_cost >= session.probe->group_bytes ||
              frame.packets[i].metadata.extended_sequence == session.probe->last_sequence) break;
        }
        if (group_cost > allowance) {
          const auto funded_at = p.earliest(session, group_cost, now);
          if (p.packet_fits(session, group_cost) && funded_at < frame.deadline_us &&
              funded_at <= add_time(session.stats.probe.next_send_us, probe_schedule_slack_us)) {
            // Do not spend a partial group on ordinary media and then claim
            // a slower synthetic probe. Wait only inside the existing slack.
            p.note_wakeup(result.next_wakeup_us, funded_at);
            continue;
          }
          p.end_probe(session, paced_probe_result_e::budget_deferred);
        }
      }
      for (std::size_t i = queued.cursor; i < frame.packets.size() && views.size() < p.bounds.maximum_batch_packets; ++i) {
        const auto &packet = frame.packets[i];
        if (packet.metadata.ip_bytes > allowance - scheduled_bytes ||
            (!views.empty() && packet.metadata.ip_bytes > p.bounds.batch_quantum_ip_bytes - std::min(scheduled_bytes, p.bounds.batch_quantum_ip_bytes))) break;
        views.push_back({ packet.udp_payload, packet.metadata, frame.deadline_us });
        if (session.probe) views.back().metadata.probe = session.probe->metadata;
        scheduled_bytes += packet.metadata.ip_bytes;
        if (session.probe && (session.probe->group_sent_bytes + scheduled_bytes >= session.probe->group_bytes ||
                               packet.metadata.extended_sequence == session.probe->last_sequence)) break;
      }
      if (views.empty()) continue;
      // Allocate our result storage before any OS submission. A successful
      // datagram must not be lost because accounting allocates afterward.
      result.successful.reserve(views.size());
      result.attempted = true;
      paced_batch_submission_t submission;
      p.busy = true;
      try {
        submission = sender(handle, views);
      }
      catch (...) {
        p.busy = false;
        p.close_accounting(handle, result);
        p.cursor = (index + 1) % count;
        return result;
      }
      p.busy = false;
      auto timestamp = now;
      bool contract_valid = submission.packets.size() == views.size() && submission.completed_at_us >= now;
      contract_valid &= !submission.suffix_budget_deferred || (submission.submission_known && submission.failed_suffix_retryable);
      contract_valid &= !submission.suffix_probe_cancelled || (session.probe && submission.submission_known &&
                                                                submission.failed_suffix_retryable && !submission.suffix_budget_deferred);
      if (contract_valid) {
        for (const auto &packet : submission.packets) {
          if (packet.submitted) {
            if (packet.send_time_us < timestamp || packet.send_time_us > submission.completed_at_us) contract_valid = false;
            timestamp = packet.send_time_us;
          }
        }
      }
      if (!contract_valid) {
        p.close_accounting(handle, result);
        p.cursor = (index + 1) % count;
        return result;
      }
      bool failed = false, success_after_failure = false, late = false;
      const auto original_cursor = queued.cursor;
      std::size_t successes = 0;
      for (std::size_t i = 0; i < submission.packets.size(); ++i) {
        const auto &sent = submission.packets[i];
        if (!sent.submitted) {
          failed = true;
          continue;
        }
        success_after_failure |= failed;
        auto &packet = frame.packets[original_cursor + i];
        p.advance_session(session, sent.send_time_us);
        if (p.host) p.host->advance(sent.send_time_us);
        if (!p.debit(session, packet.metadata.ip_bytes)) {
          session.stats.accounting_valid = false;
          p.host_stats.accounting_valid = false;
        }
        packet.metadata.send_time_us = sent.send_time_us;
        // The views carry a plan; only confirmed OS receipts acquire its tag.
        packet.metadata.probe = views[i].metadata.probe;
        result.successful.push_back({ handle, frame.policy, packet.metadata, frame.deadline_us });
        ++successes;
        ++queued.submitted_packets;
        if (packet.metadata.kind == packet_kind_e::data) {
          ++queued.submitted_data_packets;
          queued.late_data |= sent.send_time_us >= frame.deadline_us;
        }
        queued.submitted_ip_bytes += packet.metadata.ip_bytes;
        queued.remaining_ip_bytes -= packet.metadata.ip_bytes;
        session.stats.accounting_valid &= increment(session.stats.submitted_packets, 1);
        session.stats.accounting_valid &= increment(session.stats.submitted_ip_bytes, packet.metadata.ip_bytes);
        p.host_stats.accounting_valid &= increment(p.host_stats.submitted_packets, 1);
        p.host_stats.accounting_valid &= increment(p.host_stats.submitted_ip_bytes, packet.metadata.ip_bytes);
        p.payload_bytes -= packet.udp_payload.size();
        session.stats.queued_payload_bytes -= packet.udp_payload.size();
        session.stats.queued_ip_bytes -= packet.metadata.ip_bytes;
        --p.queued_packets;
        --session.stats.queued_packets;
        std::vector<std::uint8_t>().swap(packet.udp_payload);
        late |= sent.send_time_us >= frame.deadline_us;
      }
      p.clock_us = submission.completed_at_us;
      p.advance_session(session, p.clock_us);
      if (p.host) p.host->advance(p.clock_us);
      p.cursor = (index + 1) % count;
      if (session.probe) {
        auto &probe = session.stats.probe;
        probe.successful_packets += successes;
        std::uint64_t group_success_bytes = 0;
        for (const auto &success : result.successful) group_success_bytes += success.packet.ip_bytes;
        probe.successful_ip_bytes += group_success_bytes;
        session.probe->group_sent_bytes += group_success_bytes;
        const bool last_packet_sent = successes && result.successful.back().packet.extended_sequence == session.probe->last_sequence;
        const bool group_complete = session.probe->group_sent_bytes >= session.probe->group_bytes || last_packet_sent;
        if (group_complete) ++probe.successful_groups;
        if (late || p.clock_us >= frame.deadline_us)
          p.end_probe(session, paced_probe_result_e::deadline);
        else if (submission.suffix_probe_cancelled)
          p.end_probe(session, paced_probe_result_e::cancelled);
        else if (failed || !submission.submission_known)
          p.end_probe(session, submission.suffix_budget_deferred ? paced_probe_result_e::budget_deferred : paced_probe_result_e::send_failed);
        else if (last_packet_sent)
          p.end_probe(session, paced_probe_result_e::complete);
        else if (group_complete) {
          probe.next_send_us = add_time(p.clock_us, std::max(static_cast<std::uint64_t>(session.probe->delta_us),
                                                      ceil_div(session.probe->group_sent_bytes * 8000, static_cast<std::uint64_t>(session.probe->metadata.send_kbps))));
          session.probe->group_sent_bytes = 0;
        }
        // A group may span several bounded submissions. Retain its original
        // schedule/slack until complete, rotate normally, and retry immediately
        // after peers get their turn. No permit or extra credit is retained.
      }
      if (!submission.submission_known || !session.stats.accounting_valid || !p.host_stats.accounting_valid) {
        // Successful records are still returned; an arithmetic/accounting
        // failure must not permit further unprovable budget enforcement.
        if (success_after_failure) {
          auto outcome = p.finish(handle, session, frame_send_result_e::stopped);
          outcome.abandoned_packets -= successes;
          result.frames.push_back(std::move(outcome));
        }
        else
          queued.cursor += successes;
        p.close_accounting(submission.submission_known ? std::optional<std::uint64_t> {} : handle, result);
        return result;
      }
      const bool deferred = submission.suffix_budget_deferred || submission.suffix_probe_cancelled;
      if (failed && !deferred) ++queued.failed_attempts;
      if (success_after_failure) {
        // Move successes out of the abandoned count, even when the platform
        // fallback has holes. No unsuccessful old identity will be retried
        // after a later successful identity has reached the send ledger.
        auto outcome = p.finish(handle, session, late || p.clock_us >= frame.deadline_us ? frame_send_result_e::deadline_expired : frame_send_result_e::send_failed);
        outcome.abandoned_packets -= successes;
        result.frames.push_back(std::move(outcome));
      }
      else {
        queued.cursor += successes;
        if (late)
          result.frames.push_back(p.finish(handle, session, frame_send_result_e::deadline_expired));
        else if (queued.cursor == frame.packets.size())
          result.frames.push_back(p.finish(handle, session, frame_send_result_e::complete));
        else if (p.clock_us >= frame.deadline_us)
          result.frames.push_back(p.finish(handle, session, frame_send_result_e::deadline_expired));
        else if (failed && !deferred && (!submission.failed_suffix_retryable || queued.failed_attempts >= p.bounds.maximum_failed_attempts))
          result.frames.push_back(p.finish(handle, session, frame_send_result_e::send_failed));
        else if (failed)
          session.retry_at_us = add_time(p.clock_us, static_cast<std::uint64_t>(p.bounds.retry_delay_us));
      }
      if (!session.frames.empty()) p.note_wakeup(result.next_wakeup_us,
        std::max({ p.clock_us, session.retry_at_us, session.probe ? session.stats.probe.next_send_us : p.clock_us }));
      // Other sessions may have become eligible during the socket call.
      if (count > 1) p.note_wakeup(result.next_wakeup_us, p.clock_us);
      return result;
    }
    return result;
  }

  paced_probe_result_e
  deadline_pacer_t::start_probe(std::uint64_t handle, const paced_probe_request_t &request, std::int64_t now) {
    auto &p = *impl_;
    const auto found = p.sessions.find(handle);
    if (p.busy || p.stopped || found == p.sessions.end() || found->second.stats.stopped ||
        now < p.clock_us || now < 0 || now > std::numeric_limits<std::int64_t>::max() / 4 ||
        request.cluster_id < 0 || request.target_kbps <= 0 || request.target_kbps > 800000 ||
        request.duration_us <= 0 || request.duration_us > 200000 ||
        request.minimum_delta_us <= 0 || request.minimum_delta_us > 20000 ||
        request.minimum_packets < 2 || request.minimum_packets > 32) return paced_probe_result_e::invalid;
    auto &session = found->second;
    if (session.probe) return paced_probe_result_e::busy;
    if (request.cluster_id <= session.last_probe_cluster) return paced_probe_result_e::invalid;
    if (session.frames.empty()) return paced_probe_result_e::insufficient_media;
    const auto min_bytes = ceil_div(static_cast<std::uint64_t>(request.target_kbps * request.duration_us), 8000);
    const auto group_bytes = ceil_div(static_cast<std::uint64_t>(request.target_kbps * request.minimum_delta_us), 8000);
    if (!p.packet_fits(session, group_bytes)) return paced_probe_result_e::budget_deferred;
    const auto &queued = session.frames.front();
    const auto &frame = queued.frame;
    std::uint64_t bytes = 0, group = 0, last_sequence = 0, span = 0;
    std::uint32_t groups = 0;
    bool ready = false;
    for (auto i = queued.cursor; i < frame.packets.size(); ++i) {
      const auto &packet = frame.packets[i].metadata;
      if (packet.probe.cluster_id >= 0) return paced_probe_result_e::invalid;
      bytes += packet.ip_bytes;
      group += packet.ip_bytes;
      if (group >= group_bytes) {
        if (!p.packet_fits(session, group)) return paced_probe_result_e::budget_deferred;
        ++groups;
        if (bytes >= min_bytes && groups >= static_cast<std::uint32_t>(request.minimum_packets)) {
          last_sequence = packet.extended_sequence;
          ready = true;
          break;
        }
        span += std::max(static_cast<std::uint64_t>(request.minimum_delta_us),
          ceil_div(group * 8000, static_cast<std::uint64_t>(request.target_kbps)));
        group = 0;
      }
    }
    if (!ready) return paced_probe_result_e::insufficient_media;
    if (add_time(p.earliest(session, queued.remaining_ip_bytes, now), span + probe_schedule_slack_us) >= frame.deadline_us)
      return paced_probe_result_e::deadline;
    // Admission does not promise future credit: shared/host outlets may consume
    // it. Actual dispatch rechecks all buckets and the adapter's shared permit.
    if (!p.accept_time(now)) return paced_probe_result_e::invalid;
    session.probe = impl_t::session_t::probe_t {
      { request.cluster_id, request.minimum_packets, static_cast<std::int32_t>(min_bytes), static_cast<std::int32_t>(request.target_kbps) },
      frame.frame_id, group_bytes, last_sequence, request.minimum_delta_us
    };
    session.last_probe_cluster = request.cluster_id;
    session.stats.probe = { request.cluster_id, paced_probe_result_e::active, 0, 0, 0, now };
    return paced_probe_result_e::active;
  }

  bool
  deadline_pacer_t::cancel_probe(std::uint64_t handle, std::int64_t now) {
    auto &p = *impl_;
    const auto found = p.sessions.find(handle);
    if (found == p.sessions.end() || !p.accept_time(now)) return false;
    p.end_probe(found->second, paced_probe_result_e::cancelled);
    return true;
  }

  std::vector<paced_frame_result_t>
  deadline_pacer_t::stop_session(std::uint64_t handle, std::int64_t now) {
    auto &p = *impl_;
    std::vector<paced_frame_result_t> results;
    if (p.busy) return results;
    (void) p.accept_time(now);
    const auto it = p.sessions.find(handle);
    if (it == p.sessions.end()) return results;
    auto &session = it->second;
    while (!session.frames.empty()) results.push_back(p.finish(handle, session, frame_send_result_e::stopped));
    p.sessions.erase(it);
    const auto where = std::find(p.order.begin(), p.order.end(), handle);
    if (where != p.order.end()) {
      const auto position = static_cast<std::size_t>(where - p.order.begin());
      p.order.erase(where);
      if (position < p.cursor) --p.cursor;
      if (p.order.empty())
        p.cursor = 0;
      else
        p.cursor %= p.order.size();
    }
    return results;
  }

  std::vector<paced_frame_result_t>
  deadline_pacer_t::stop(std::int64_t now) {
    auto &p = *impl_;
    std::vector<paced_frame_result_t> results;
    if (p.busy) return results;
    (void) p.accept_time(now);
    for (auto &[handle, session] : p.sessions) {
      while (!session.frames.empty()) results.push_back(p.finish(handle, session, frame_send_result_e::stopped));
      session.stats.stopped = true;
    }
    p.stopped = true;
    return results;
  }

  bool
  deadline_pacer_t::external_allowance(std::uint64_t handle, std::uint64_t bytes, std::int64_t now) {
    auto &p = *impl_;
    auto it = p.sessions.find(handle);
    if (p.stopped || it == p.sessions.end() || it->second.stats.stopped || bytes == 0 || bytes > maximum_credit_bytes || !p.accept_time(now)) return false;
    p.advance_session(it->second, now);
    return bytes <= p.allowance(it->second);
  }

  bool
  deadline_pacer_t::abort_noexcept() noexcept {
    auto &p = *impl_;
    if (p.busy) return false;
    p.stopped = true;
    p.host_stats.accounting_valid = false;
    p.host_stats.stopped = true;
    for (auto &[handle, session] : p.sessions) {
      (void) handle;
      session.frames.clear();
      session.stats.queued_frames = 0;
      session.stats.queued_packets = 0;
      session.stats.queued_payload_bytes = 0;
      session.stats.queued_ip_bytes = 0;
      session.stats.accounting_valid = false;
      session.stats.stopped = true;
    }
    p.queued_packets = 0;
    p.payload_bytes = 0;
    return true;
  }

  bool
  deadline_pacer_t::debit_external_success(std::uint64_t handle, std::uint64_t bytes, std::int64_t now) {
    auto &p = *impl_;
    auto it = p.sessions.find(handle);
    if (p.stopped || it == p.sessions.end() || it->second.stats.stopped || bytes == 0 || bytes > maximum_credit_bytes || !p.accept_time(now)) return false;
    auto &session = it->second;
    p.advance_session(session, now);
    const auto allowed = p.debit(session, bytes);
    session.stats.accounting_valid &= increment(session.stats.externally_submitted_ip_bytes, bytes);
    p.host_stats.accounting_valid &= increment(p.host_stats.externally_submitted_ip_bytes, bytes);
    if (!allowed || !session.stats.accounting_valid || !p.host_stats.accounting_valid) {
      session.stats.accounting_valid = false;
      p.host_stats.accounting_valid = false;
      p.stopped = true;
      for (auto &[other_handle, other] : p.sessions) {
        (void) other_handle;
        other.stats.stopped = true;
      }
    }
    return allowed && session.stats.accounting_valid && p.host_stats.accounting_valid;
  }

  std::optional<pacer_snapshot_t>
  deadline_pacer_t::snapshot(std::uint64_t handle) const {
    const auto &p = *impl_;
    const auto it = p.sessions.find(handle);
    if (it == p.sessions.end()) return {};
    auto result = it->second.stats;
    result.budget_debt_bytes = it->second.buckets.budget.debt();
    result.instant_debt_bytes = it->second.buckets.instant.debt();
    return result;
  }

  pacer_snapshot_t
  deadline_pacer_t::host_snapshot() const {
    const auto &p = *impl_;
    auto result = p.host_stats;
    result.queued_payload_bytes = p.payload_bytes;
    result.queued_packets = p.queued_packets;
    for (const auto &[handle, session] : p.sessions) {
      (void) handle;
      result.queued_frames += session.frames.size();
      result.queued_ip_bytes += session.stats.queued_ip_bytes;
    }
    result.stopped = p.stopped;
    if (p.host) {
      result.budget_debt_bytes = p.host->budget.debt();
      result.instant_debt_bytes = p.host->instant.debt();
    }
    return result;
  }
}  // namespace transport
