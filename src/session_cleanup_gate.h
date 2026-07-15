/**
 * @file src/session_cleanup_gate.h
 * @brief Coordinates client cleanup with concurrent launch operations.
 */
#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>

namespace rtsp_stream {
  class session_cleanup_gate_t {
  public:
    enum class cleanup_claim_e {
      none,
      cancelled,
      acquired,
    };

    void
    begin_launch();

    bool
    end_launch();

    bool
    request_cleanup();

    void
    cancel_cleanup();

    cleanup_claim_e
    claim_cleanup(bool host_idle);

    void
    finish_cleanup();

  private:
    std::mutex _mutex;
    std::condition_variable _state_changed;
    std::size_t _launches_in_flight {};
    bool _cleanup_requested { false };
    bool _cleanup_running { false };
  };
}  // namespace rtsp_stream
