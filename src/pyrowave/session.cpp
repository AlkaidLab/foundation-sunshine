/**
 * @file src/pyrowave/session.cpp
 * @brief Per-stream state transitions for the experimental PyroWave path.
 */
#include "session.h"

namespace pyrowave {

  failure_e
  session_t::negotiate(
    const config_t &config,
    const device_capabilities_t &device,
    const client_capabilities_t &client) noexcept {
    if (snapshot_.state != session_state_e::idle && snapshot_.state != session_state_e::failed) {
      snapshot_.failure = failure_e::invalid_state;
      snapshot_.state = session_state_e::failed;
      snapshot_.last_frame_id = 0;
      return snapshot_.failure;
    }

    snapshot_.state = session_state_e::negotiating;
    const auto result = evaluate_capabilities(config, device, client);
    if (result.availability != availability_e::available) {
      snapshot_.failure = result.failure;
      if (result.availability == availability_e::disabled && snapshot_.failure == failure_e::none) {
        snapshot_.failure = failure_e::feature_disabled;
      }
      if (snapshot_.failure == failure_e::none) {
        snapshot_.failure = failure_e::client_unsupported;
      }
      snapshot_.state = session_state_e::failed;
      snapshot_.last_frame_id = 0;
      return snapshot_.failure;
    }

    snapshot_.failure = failure_e::none;
    snapshot_.state = session_state_e::prepared;
    return failure_e::none;
  }

  failure_e
  session_t::start() noexcept {
    if (snapshot_.state != session_state_e::prepared) {
      snapshot_.failure = failure_e::invalid_state;
      snapshot_.state = session_state_e::failed;
      snapshot_.last_frame_id = 0;
      return snapshot_.failure;
    }

    snapshot_.failure = failure_e::none;
    snapshot_.state = session_state_e::streaming;
    return failure_e::none;
  }

  void
  session_t::stop() noexcept {
    if (snapshot_.state == session_state_e::idle) {
      return;
    }

    snapshot_.state = session_state_e::stopping;
    snapshot_.state = session_state_e::idle;
    snapshot_.failure = failure_e::none;
    snapshot_.last_frame_id = 0;
  }

}  // namespace pyrowave
