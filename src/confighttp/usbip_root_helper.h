/**
 * @file src/confighttp/usbip_root_helper.h
 * @brief Time limits, one repair at a time, and a short status cache for the usbip root helper.
 *
 * The Windows HTTP handlers are not built on this host. The decisions live here so a test can
 * compile them on their own: how long a status or a repair may run, when a second repair is
 * rejected, when a status is answered from the previous result, and how a finite wait becomes
 * the millisecond argument of a single-object wait.
 *
 * The wait is WaitForSingleObject (synchapi.h) with dwMilliseconds. INFINITE is 0xFFFFFFFF and
 * would wait without a bound, so a remaining time that does not fit is capped at 0xFFFFFFFE and
 * a negative remaining time becomes zero. Boost.Process 1.91 marks child::wait_for unreliable;
 * it is not used. On timeout the child is stopped with TerminateProcess on that process handle
 * only. TerminateProcess is asynchronous, so the same handle is waited on again for a short reap.
 * Other processes are left alone.
 *
 * Microsoft Learn, WaitForSingleObject function (synchapi.h), retrieved 2026-11-07.
 * Microsoft Learn, TerminateProcess function (processthreadsapi.h), retrieved 2026-11-07.
 */
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <istream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace confighttp::usbip_root {
  inline constexpr std::chrono::milliseconds status_budget { 15000 };
  inline constexpr std::chrono::milliseconds repair_budget { 60000 };
  /** A status answer may be returned again for this long instead of starting PowerShell. */
  inline constexpr std::chrono::milliseconds status_reuse { 5000 };
  /** After the child is stopped, how long the caller waits for its stdout reader to finish. */
  inline constexpr std::chrono::milliseconds reader_grace { 2000 };
  /** TerminateProcess returns before the process is signaled. This reap stays finite. */
  inline constexpr std::chrono::milliseconds reap_budget { 2000 };

  /** Largest dwMilliseconds that is not INFINITE (0xFFFFFFFF). */
  inline constexpr unsigned long finite_wait_limit = 0xfffffffeu;

  inline constexpr const char *fixed_powershell_arguments[] = {
    "-NoProfile",
    "-ExecutionPolicy",
    "Bypass",
    "-File",
  };

  enum class action {
    status,
    repair,
    unknown,
  };

  inline action
  parse_action(std::string_view text) noexcept {
    if (text == "status") return action::status;
    if (text == "repair") return action::repair;
    return action::unknown;
  }

  inline const char *
  action_argument(action which) noexcept {
    return which == action::repair ? "repair" : "status";
  }

  inline std::chrono::milliseconds
  budget_for(action which) noexcept {
    if (which == action::repair) return repair_budget;
    if (which == action::status) return status_budget;
    return std::chrono::milliseconds::zero();
  }

  /**
   * Milliseconds for WaitForSingleObject. Zero polls. Never returns INFINITE.
   */
  inline unsigned long
  wait_milliseconds(std::chrono::milliseconds remaining) noexcept {
    if (remaining.count() <= 0) return 0;
    constexpr auto limit = static_cast<std::chrono::milliseconds::rep>(finite_wait_limit);
    if (remaining.count() >= limit) return finite_wait_limit;
    return static_cast<unsigned long>(remaining.count());
  }

  enum class wait_outcome {
    exited,
    timed_out,
    failed,
  };

  inline bool
  should_terminate(wait_outcome outcome) noexcept {
    return outcome != wait_outcome::exited;
  }

  struct failure_note {
    const char *error;
    const char *ds4_fallback;
    bool busy;
    bool timed_out;
  };

  inline failure_note
  timeout_failure() noexcept {
    return {
      "usbip-win2 root helper timed out",
      "DS4 fallback remains active because the usbip-win2 helper timed out.",
      false,
      true,
    };
  }

  inline failure_note
  busy_failure() noexcept {
    return {
      "usbip-win2 root repair is already running",
      "DS4 fallback remains active because a usbip-win2 repair is already running.",
      true,
      false,
    };
  }

  /** The only extra JSON member a failure note adds. Success answers add neither. */
  inline const char *
  extra_flag(const failure_note &note) noexcept {
    if (note.busy) return "busy";
    if (note.timed_out) return "timed_out";
    return nullptr;
  }

  struct bounded_result {
    wait_outcome outcome { wait_outcome::failed };
    int kill_calls { 0 };
    std::string last_line;
    bool reader_finished { false };
    /** Stdout was still blocked. A detached thread joins the reader and keeps keep_alive. */
    bool handed_off { false };
  };

  /**
   * Read stdout beside the budget. wait_fn receives a finite millisecond count and must
   * not wait longer than that. Unless the child has exited, kill_fn runs once and must
   * stop only that child. keep_alive owns the stream if the reader is handed off.
   */
  template <class WaitFn, class KillFn>
  bounded_result
  read_bounded(std::istream &input,
    std::shared_ptr<void> keep_alive,
    std::chrono::milliseconds budget,
    WaitFn &&wait_fn,
    KillFn &&kill_fn,
    std::chrono::milliseconds grace) {
    struct state_t {
      std::mutex mutex;
      std::condition_variable ready;
      std::string last_line;
      bool done { false };
    };
    auto state = std::make_shared<state_t>();
    std::thread reader([state, &input] {
      struct mark_t {
        state_t *state;
        ~mark_t() {
          {
            std::lock_guard lock(state->mutex);
            state->done = true;
          }
          state->ready.notify_all();
        }
      } mark { state.get() };
      std::string line;
      while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        std::lock_guard lock(state->mutex);
        state->last_line = std::move(line);
      }
    });

    auto finish_reader = [&](bool finished) {
      if (finished) {
        if (reader.joinable()) reader.join();
        return false;
      }
      if (reader.joinable()) {
        std::thread([keep_alive, reader = std::move(reader)]() mutable {
          reader.join();
        }).detach();
        return true;
      }
      return false;
    };

    bounded_result result;
    try {
      result.outcome = wait_fn(wait_milliseconds(budget));
      if (should_terminate(result.outcome)) {
        result.kill_calls = 1;
        kill_fn();
      }
    }
    catch (...) {
      if (result.kill_calls == 0) {
        result.kill_calls = 1;
        try {
          kill_fn();
        }
        catch (...) {
        }
      }
      bool finished = false;
      {
        std::unique_lock lock(state->mutex);
        finished = state->ready.wait_for(lock, grace, [&] { return state->done; });
      }
      finish_reader(finished);
      throw;
    }

    bool finished = false;
    {
      std::unique_lock lock(state->mutex);
      finished = state->ready.wait_for(lock, grace, [&] { return state->done; });
      result.last_line = state->last_line;
      result.reader_finished = finished;
    }
    result.handed_off = finish_reader(finished);
    return result;
  }

  struct steady_clock_fn {
    std::chrono::steady_clock::time_point
    operator()() const {
      return std::chrono::steady_clock::now();
    }
  };

  template <class NowFn = steady_clock_fn>
  class gate {
  public:
    enum class repair_take {
      granted,
      busy,
    };
    enum class status_take {
      start,
      reuse,
      gave_up,
    };
    struct status_ticket {
      status_take take { status_take::gave_up };
      std::string body;
    };

    explicit gate(NowFn now = {}): now_(std::move(now)) {}

    gate(const gate &) = delete;
    gate &
    operator=(const gate &) = delete;

    repair_take
    begin_repair(std::chrono::milliseconds wait_for_status) {
      std::unique_lock lock(mutex_);
      const auto deadline = std::chrono::steady_clock::now() + wait_for_status;
      while (status_running_) {
        if (ready_.wait_until(lock, deadline) == std::cv_status::timeout) return repair_take::busy;
      }
      if (repair_running_) return repair_take::busy;
      repair_running_ = true;
      return repair_take::granted;
    }

    void
    finish_repair(std::string status_body) {
      std::lock_guard lock(mutex_);
      repair_running_ = false;
      store_unlocked(std::move(status_body));
      ready_.notify_all();
    }

    void
    cancel_repair() {
      std::lock_guard lock(mutex_);
      repair_running_ = false;
      ready_.notify_all();
    }

    status_ticket
    acquire_status(std::chrono::milliseconds bound) {
      std::unique_lock lock(mutex_);
      const auto deadline = std::chrono::steady_clock::now() + bound;
      for (;;) {
        if (fresh_unlocked(now_())) return { status_take::reuse, status_body_ };
        if (!status_running_ && !repair_running_) {
          status_running_ = true;
          return { status_take::start, {} };
        }
        if (std::chrono::steady_clock::now() >= deadline) return { status_take::gave_up, {} };
        ready_.wait_until(lock, deadline);
      }
    }

    void
    finish_status(std::string body) {
      std::lock_guard lock(mutex_);
      status_running_ = false;
      store_unlocked(std::move(body));
      ready_.notify_all();
    }

    void
    cancel_status() {
      std::lock_guard lock(mutex_);
      status_running_ = false;
      ready_.notify_all();
    }

    void
    drop_status_cache() {
      std::lock_guard lock(mutex_);
      has_status_ = false;
      status_body_.clear();
    }

  private:
    bool
    fresh_unlocked(std::chrono::steady_clock::time_point now) const {
      return has_status_ && (now - status_at_) < status_reuse;
    }

    void
    store_unlocked(std::string body) {
      if (body.empty()) return;
      status_body_ = std::move(body);
      status_at_ = now_();
      has_status_ = true;
    }

    NowFn now_;
    std::mutex mutex_;
    std::condition_variable ready_;
    bool repair_running_ { false };
    bool status_running_ { false };
    bool has_status_ { false };
    std::chrono::steady_clock::time_point status_at_ {};
    std::string status_body_;
  };

  inline gate<> &
  shared_gate() {
    static gate<> instance;
    return instance;
  }
}  // namespace confighttp::usbip_root
