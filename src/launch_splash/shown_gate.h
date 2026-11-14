/**
 * @file src/launch_splash/shown_gate.h
 * @brief The wait of an app start for the Panel to have the launch splash on the streamed display.
 *
 * The launch is announced to the Panel when the request is accepted; the app command starts only after the Panel says the splash is on the display with its
 * first frame painted, but never later than a bound after it began waiting. Without a Panel that can answer there is nothing to wait for. Self-contained, so the
 * rules (answered, timed out, nobody listening, withdrawn) are tested on any platform.
 */
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>

namespace launch_splash {

  class shown_gate_t {
  public:
    enum class how_e {
      answered,  ///< the Panel had the splash on screen (maybe before the wait began)
      timed_out,  ///< the bound passed: the app starts anyway
      no_listener,  ///< no Panel that can answer is connected: nothing was waited for
      withdrawn,  ///< the launch ended or was replaced while waiting
    };

    struct outcome_t {
      how_e how = how_e::no_listener;
      std::int64_t panel_ms = 0;  ///< what the Panel measured: display ready to first frame
      std::int64_t prepare_ms = 0;  ///< what the Panel measured: announcement to window ready
      std::int64_t waited_ms = 0;  ///< how long the wait itself took
    };

    /// A launch whose answer is awaited (every announced or begun launch); an answer of an older one is no answer.
    void
    arm(const std::string &id) {
      std::lock_guard lock { mutex_ };
      id_ = id;
      answered_ = false;
      withdrawn_ = false;
      panel_ms_ = prepare_ms_ = 0;
    }

    /// The Panel's answer. False for any launch but the armed one, and for a second answer.
    bool
    note(const std::string &id, const std::int64_t panel_ms, const std::int64_t prepare_ms) {
      {
        std::lock_guard lock { mutex_ };
        if (id.empty() || id != id_ || answered_) {
          return false;
        }
        answered_ = true;
        panel_ms_ = panel_ms;
        prepare_ms_ = prepare_ms;
      }
      cv_.notify_all();
      return true;
    }

    /// The launch ended or was withdrawn: a wait on it returns at once.
    void
    withdraw() {
      {
        std::lock_guard lock { mutex_ };
        withdrawn_ = true;
      }
      cv_.notify_all();
    }

    outcome_t
    wait(const std::string &id, const std::chrono::milliseconds bound, const bool listener) {
      std::unique_lock lock { mutex_ };
      const auto began = std::chrono::steady_clock::now();
      outcome_t out;
      const auto finish = [&](const how_e how) {
        out.how = how;
        out.panel_ms = panel_ms_;
        out.prepare_ms = prepare_ms_;
        out.waited_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count();
        return out;
      };
      if (id != id_ || withdrawn_) {
        return finish(how_e::withdrawn);
      }
      if (answered_) {
        return finish(how_e::answered);
      }
      if (!listener) {
        return finish(how_e::no_listener);
      }
      cv_.wait_for(lock, bound, [&] { return answered_ || withdrawn_ || id != id_; });
      if (id != id_ || (withdrawn_ && !answered_)) {
        return finish(how_e::withdrawn);
      }
      return finish(answered_ ? how_e::answered : how_e::timed_out);
    }

  private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::string id_;
    bool answered_ = false;
    bool withdrawn_ = false;
    std::int64_t panel_ms_ = 0;
    std::int64_t prepare_ms_ = 0;
  };

}  // namespace launch_splash
