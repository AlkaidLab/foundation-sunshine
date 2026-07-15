/**
 * @file src/session_cleanup_gate.cpp
 * @brief Coordinates client cleanup with concurrent launch operations.
 */

#include "session_cleanup_gate.h"

#include <cassert>

namespace rtsp_stream {
  void
  session_cleanup_gate_t::begin_launch() {
    std::unique_lock lock { _mutex };
    _state_changed.wait(lock, [this]() {
      return !_cleanup_requested && !_cleanup_running;
    });
    ++_launches_in_flight;
  }

  bool
  session_cleanup_gate_t::end_launch() {
    std::lock_guard lock { _mutex };
    assert(_launches_in_flight != 0);
    --_launches_in_flight;
    return _cleanup_requested && _launches_in_flight == 0;
  }

  bool
  session_cleanup_gate_t::request_cleanup() {
    std::lock_guard lock { _mutex };
    if (_cleanup_running) {
      return false;
    }

    _cleanup_requested = true;
    return _launches_in_flight == 0;
  }

  void
  session_cleanup_gate_t::cancel_cleanup() {
    {
      std::lock_guard lock { _mutex };
      if (!_cleanup_requested) {
        return;
      }
      _cleanup_requested = false;
    }
    _state_changed.notify_all();
  }

  session_cleanup_gate_t::cleanup_claim_e
  session_cleanup_gate_t::claim_cleanup(bool host_idle) {
    std::lock_guard lock { _mutex };
    if (!_cleanup_requested || _cleanup_running) {
      return cleanup_claim_e::none;
    }
    if (!host_idle) {
      _cleanup_requested = false;
      _state_changed.notify_all();
      return cleanup_claim_e::cancelled;
    }
    if (_launches_in_flight != 0) {
      return cleanup_claim_e::none;
    }

    _cleanup_requested = false;
    _cleanup_running = true;
    return cleanup_claim_e::acquired;
  }

  void
  session_cleanup_gate_t::finish_cleanup() {
    {
      std::lock_guard lock { _mutex };
      assert(_cleanup_running);
      _cleanup_running = false;
    }
    _state_changed.notify_all();
  }
}  // namespace rtsp_stream
