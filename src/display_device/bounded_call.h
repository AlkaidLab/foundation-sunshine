/**
 * @file src/display_device/bounded_call.h
 * @brief Run a call that may never return on its own thread and wait for it a bounded time.
 *
 * The display stack (CCD: DisplayConfigGetDeviceInfo and its neighbours) can block for minutes inside the graphics driver, for instance while a
 * display is being switched or a driver is resetting. A caller that must not wait forever (the start of the host, with the web UI not yet bound)
 * hands the call to this helper: it returns whether the call finished, and a call that did not keeps its thread until the driver answers.
 */
#pragma once

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace display_device {

  /**
   * @brief Run `call` on a detached thread and wait at most `limit` for it.
   * @param call Must own what it uses (statics, shared pointers, values): it can outlive the caller. A thrown exception is swallowed here.
   * @returns True when the call finished within the limit, false when it is still running (it runs on).
   */
  template <class Call>
  bool
  run_bounded(Call call, std::chrono::milliseconds limit) {
    struct state_t {
      std::mutex mutex;
      std::condition_variable cv;
      bool done = false;
    };
    auto state = std::make_shared<state_t>();
    std::thread([state, call = std::move(call)]() mutable {
      try {
        call();
      }
      catch (...) {
      }
      std::lock_guard lock { state->mutex };
      state->done = true;
      state->cv.notify_all();
    }).detach();
    std::unique_lock lock { state->mutex };
    return state->cv.wait_for(lock, limit, [&] { return state->done; });
  }

}  // namespace display_device
