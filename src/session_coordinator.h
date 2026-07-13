/**
 * @file src/session_coordinator.h
 * @brief Serialized active-session lifecycle observations.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <boost/asio/any_io_executor.hpp>

namespace session_coord {
  constexpr std::size_t MAX_CLOSED_TOMBSTONES = 256;

  enum class state_e : int {
    // State order is monotonic. Events may advance but never rewind a record.
    starting,
    active,
    draining,
    closed,
  };

  const char *
  state_name(state_e state) noexcept;

  struct event_t {
    std::uint32_t session_id {};
    std::string client_uuid;
    state_e state { state_e::starting };
    std::string stop_reason;
  };

  struct record_t {
    std::uint32_t session_id {};
    std::string client_uuid;
    state_e state { state_e::starting };
    std::string stop_reason;
    std::uint64_t updated_version {};
  };

  struct snapshot_t {
    std::uint64_t version {};
    std::size_t rejected_events {};
    std::vector<record_t> records;
  };

  /**
   * @brief Maintains a monotonic lifecycle view on an Asio strand.
   *
   * The coordinator is observational for now: it does not own transport or
   * media resources. Recent late lifecycle events cannot resurrect a closed
   * session, and a non-empty authenticated owner cannot be replaced by another
   * owner.
   */
  class coordinator_t {
  public:
    explicit coordinator_t(boost::asio::any_io_executor executor);
    coordinator_t(const coordinator_t &) = delete;
    coordinator_t &
    operator=(const coordinator_t &) = delete;

    void
    observe(event_t event) const noexcept;

    std::shared_ptr<const snapshot_t>
    snapshot() const;

  private:
    struct impl_t;
    std::shared_ptr<impl_t> _impl;
  };
}  // namespace session_coord
