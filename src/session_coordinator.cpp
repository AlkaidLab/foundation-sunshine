/**
 * @file src/session_coordinator.cpp
 * @brief Serialized active-session lifecycle observations.
 */

#include "session_coordinator.h"

#include <algorithm>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <utility>

#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>

namespace session_coord {
  namespace {
    int
    state_rank(state_e state) noexcept {
      return static_cast<int>(state);
    }
  }  // namespace

  const char *
  state_name(state_e state) noexcept {
    switch (state) {
      case state_e::starting:
        return "starting";
      case state_e::active:
        return "active";
      case state_e::draining:
        return "draining";
      case state_e::closed:
        return "closed";
    }
    return "unknown";
  }

  struct coordinator_t::impl_t {
    explicit impl_t(boost::asio::any_io_executor executor):
        strand { boost::asio::make_strand(std::move(executor)) },
        published { std::make_shared<const snapshot_t>() } {
    }

    void
    apply(event_t event) {
      if (event.session_id == 0) {
        ++rejected_events;
        ++version;
        publish();
        return;
      }

      auto entry = records.find(event.session_id);
      if (entry == records.end()) {
        record_t record {
          .session_id = event.session_id,
          .client_uuid = std::move(event.client_uuid),
          .state = event.state,
          .stop_reason = std::move(event.stop_reason),
          .updated_version = ++version,
        };
        const auto closed = record.state == state_e::closed;
        auto inserted = records.emplace(record.session_id, std::move(record)).first;
        if (closed) {
          remember_closed(inserted->second);
        }
        publish();
        return;
      }

      auto &record = entry->second;
      if (!record.client_uuid.empty() &&
          !event.client_uuid.empty() &&
          record.client_uuid != event.client_uuid) {
        ++rejected_events;
        ++version;
        publish();
        return;
      }

      bool changed { false };
      if (record.client_uuid.empty() && !event.client_uuid.empty()) {
        record.client_uuid = std::move(event.client_uuid);
        changed = true;
      }

      const auto previous_state = record.state;
      if (state_rank(event.state) > state_rank(record.state)) {
        record.state = event.state;
        changed = true;
      }

      if (state_rank(event.state) >= state_rank(state_e::draining) &&
          record.stop_reason.empty() &&
          !event.stop_reason.empty()) {
        record.stop_reason = std::move(event.stop_reason);
        changed = true;
      }

      if (!changed) {
        return;
      }

      record.updated_version = ++version;
      if (previous_state != state_e::closed && record.state == state_e::closed) {
        remember_closed(record);
      }
      publish();
    }

    void
    remember_closed(const record_t &record) {
      closed_order.emplace_back(record.session_id, record.updated_version);
      while (closed_order.size() > MAX_CLOSED_TOMBSTONES) {
        const auto [session_id, closed_version] = closed_order.front();
        closed_order.pop_front();

        auto old = records.find(session_id);
        if (old != records.end() &&
            old->second.state == state_e::closed &&
            old->second.updated_version == closed_version) {
          records.erase(old);
        }
      }
    }

    void
    publish() {
      auto next = std::make_shared<snapshot_t>();
      next->version = version;
      next->rejected_events = rejected_events;
      next->records.reserve(records.size());
      for (const auto &[_, record] : records) {
        next->records.push_back(record);
      }
      std::ranges::sort(next->records, {}, &record_t::session_id);

      std::lock_guard lock { published_mutex };
      published = std::move(next);
    }

    std::shared_ptr<const snapshot_t>
    snapshot() const {
      std::lock_guard lock { published_mutex };
      return published;
    }

    boost::asio::strand<boost::asio::any_io_executor> strand;
    std::unordered_map<std::uint32_t, record_t> records;
    std::deque<std::pair<std::uint32_t, std::uint64_t>> closed_order;
    std::uint64_t version {};
    std::size_t rejected_events {};

    mutable std::mutex published_mutex;
    std::shared_ptr<const snapshot_t> published;
  };

  coordinator_t::coordinator_t(boost::asio::any_io_executor executor):
      _impl { std::make_shared<impl_t>(std::move(executor)) } {
  }

  void
  coordinator_t::observe(event_t event) const noexcept {
    try {
      auto impl = _impl;
      boost::asio::post(impl->strand, [impl, event = std::move(event)]() mutable {
        impl->apply(std::move(event));
      });
    }
    catch (...) {
      // Observability must never alter the streaming lifecycle.
    }
  }

  std::shared_ptr<const snapshot_t>
  coordinator_t::snapshot() const {
    return _impl->snapshot();
  }
}  // namespace session_coord
