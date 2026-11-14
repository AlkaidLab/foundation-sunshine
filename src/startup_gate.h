/**
 * @file src/startup_gate.h
 * @brief One-shot gates for the work that finishes after the servers are listening, and the timing of the start.
 *
 * Sunshine used to bind nvhttp and rtsp only after the encoder probe (about 1.2 s on an RTX 4080) and after the input devices were opened (about 1.5 s with the
 * HID enumeration of the virtual mouse): a client that asked in that time got "connection refused". The servers now open first and the handlers that read the
 * results (serverinfo, applist, launch, resume) wait on a gate, with a bound: a stuck driver call answers 503 "starting" instead of hanging the client.
 */
#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>

namespace startup {

  /** A gate that is closed when the process starts and opened once. Waiters of every thread are released when it opens; it never closes again. */
  class gate_t {
  public:
    void
    open() {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        open_ = true;
      }
      cv_.notify_all();
    }

    bool
    is_open() const {
      std::lock_guard<std::mutex> lock(mutex_);
      return open_;
    }

    /** @return true when the gate is open (also when it opened while waiting), false when the bound ran out first. */
    template <class Rep, class Period>
    bool
    wait_for(std::chrono::duration<Rep, Period> bound) const {
      std::unique_lock<std::mutex> lock(mutex_);
      return cv_.wait_for(lock, bound, [this] { return open_; });
    }

  private:
    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    bool open_ = false;
  };

  /** The encoder probe has finished (it succeeded or failed: the servers answer from what it left either way). */
  inline gate_t &
  probe_gate() {
    static gate_t gate;
    return gate;
  }

  /** The input devices (virtual mouse, gamepad back ends) are open: a stream may start. */
  inline gate_t &
  input_gate() {
    static gate_t gate;
    return gate;
  }

  /** How long a request waits for the probe before it is told that the host is still starting. A hung driver call must not hang the client with it. */
  inline constexpr std::chrono::seconds request_wait_bound { 10 };

  /** @return true when the request may go on (both gates open within the bound, shared across the two waits). */
  inline bool
  await_stream_ready(std::chrono::milliseconds bound = std::chrono::duration_cast<std::chrono::milliseconds>(request_wait_bound)) {
    const auto deadline = std::chrono::steady_clock::now() + bound;
    if (!probe_gate().wait_for(bound)) {
      return false;
    }
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    return input_gate().wait_for(left.count() > 0 ? left : std::chrono::milliseconds::zero());
  }

  /** The XML a client gets while the host is starting: Moonlight shows its retry. */
  inline std::string
  starting_message() {
    return "Sunshine is still starting; try again in a few seconds";
  }

  /**
   * @brief Writes how long a phase of the start took, when the object leaves its scope: `[startup] input::init 1490 ms (at +1650 ms)`.
   * `at` is the time since the first phase of this process, so the log reads as a timeline whatever else it holds. Info level: it costs one line per phase.
   */
  class phase_t {
  public:
    using clock = std::chrono::steady_clock;

    template <class Log>
    phase_t(std::string name, Log log):
        name_ { std::move(name) }, begin_ { clock::now() } {
      log_ = [log](const std::string &line) { log(line); };
      origin();
    }

    ~phase_t() {
      if (log_) {
        const auto now = clock::now();
        log_("[startup] " + name_ + " " + std::to_string(ms(now - begin_)) + " ms (at +" + std::to_string(ms(now - origin())) + " ms)");
      }
    }

    phase_t(const phase_t &) = delete;
    phase_t &
    operator=(const phase_t &) = delete;

    /** The start of the first phase. */
    static clock::time_point
    origin() {
      static const clock::time_point first = clock::now();
      return first;
    }

  private:
    static long long
    ms(clock::duration d) {
      return std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
    }

    std::string name_;
    clock::time_point begin_;
    std::function<void(const std::string &)> log_;
  };

}  // namespace startup
