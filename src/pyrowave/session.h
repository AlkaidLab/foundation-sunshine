/**
 * @file src/pyrowave/session.h
 * @brief Session ownership and state boundary for PyroWave.
 */
#pragma once

#include "capabilities.h"
#include "config.h"

#include <cstdint>

namespace pyrowave {

  enum class session_state_e : std::uint8_t {
    idle,
    negotiating,
    prepared,
    streaming,
    stopping,
    failed,
  };

  struct session_snapshot_t {
    session_id_t session_id = 0;
    session_state_e state = session_state_e::idle;
    failure_e failure = failure_e::none;
    frame_id_t last_frame_id = 0;
  };

  class session_t {
  public:
    explicit session_t(session_id_t session_id) noexcept:
        snapshot_ { .session_id = session_id } {
    }

    [[nodiscard]] session_snapshot_t
    snapshot() const noexcept {
      return snapshot_;
    }

    [[nodiscard]] failure_e
    negotiate(
      const config_t &config,
      const device_capabilities_t &device,
      const client_capabilities_t &client) noexcept;

    [[nodiscard]] failure_e
    start() noexcept;

    void
    stop() noexcept;

  private:
    session_snapshot_t snapshot_ {};
  };

}  // namespace pyrowave
