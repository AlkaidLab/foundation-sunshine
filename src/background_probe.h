/**
 * @file src/background_probe.h
 * @brief The encoder probe of the start on a thread of its own, and a thread that the end of the process does not wait for longer than a bound.
 *
 * The probe talks to the graphics driver. A call that never returns owns its thread for good, so the start must not wait for it (the tray and the ready state come
 * first) and the end of the process must not join it (a thread stuck in a driver cannot be told to stop). Both rules are here, free of the host's own headers, so the
 * unit tests run them with a probe that sleeps or never returns.
 */
#pragma once

#include "src/gpu_driver_guard.h"
#include "src/startup_gate.h"

#include <chrono>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <thread>

namespace startup {

  /**
   * A thread that is joined when it finishes within a bound and let go otherwise. `finish()` returns whether it was joined; the destructor finishes with `destructor_bound`.
   * A thread that was let go keeps running on its own: whatever it touches must outlive it (the probe touches only process-wide state).
   */
  class bounded_thread_t {
  public:
    explicit bounded_thread_t(std::chrono::milliseconds destructor_bound = std::chrono::seconds(2)):
        destructor_bound_ { destructor_bound } {}

    template <class Fn>
    void
    start(Fn fn) {
      auto done = std::make_shared<std::promise<void>>();
      finished_ = done->get_future();
      thread_ = std::thread([done, fn = std::move(fn)]() mutable {
        try {
          fn();
        }
        catch (...) {
        }
        done->set_value();
      });
    }

    bool
    running() const {
      return thread_.joinable();
    }

    /** @return true when the thread had finished (it is joined), false when it was let go, or none was started. */
    bool
    finish(std::chrono::milliseconds bound) {
      if (!thread_.joinable()) return false;
      if (finished_.wait_for(bound) == std::future_status::ready) {
        thread_.join();
        return true;
      }
      thread_.detach();
      return false;
    }

    ~bounded_thread_t() {
      finish(destructor_bound_);
    }

    bounded_thread_t(const bounded_thread_t &) = delete;
    bounded_thread_t &
    operator=(const bounded_thread_t &) = delete;

  private:
    std::chrono::milliseconds destructor_bound_;
    std::thread thread_;
    std::future<void> finished_;
  };

  /**
   * The steps of one encoder probe around the probe itself: the state the Web UI and the Panel read goes to `running`, the probe runs, the state goes to `done` or `failed`
   * (an exception is a failure) and the gate that holds back the requests opens, whatever happened.
   * @param probe returns true when it found no working encoder.
   * @return whether the probe found one.
   */
  inline bool
  run_probe(const std::function<bool()> &probe, gate_t &gate, const std::function<void(gpu_guard::probe_e)> &set_state) {
    set_state(gpu_guard::probe_e::running);
    bool failed = true;
    try {
      failed = probe();
    }
    catch (...) {
    }
    set_state(failed ? gpu_guard::probe_e::failed : gpu_guard::probe_e::done);
    gate.open();
    return !failed;
  }

}  // namespace startup
