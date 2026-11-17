/**
 * @file src/audio_default_wait.h
 * @brief What the host does while the system has no default playback device: say why once, keep streaming without sound,
 *        and start capturing when a device appears. No audio API is touched here, so the rules are tested alone.
 *
 * A computer without speakers, headphones or a virtual output (a server, a cloud machine, a VM) has no default playback
 * device: `IMMDeviceEnumerator::GetDefaultAudioEndpoint` fails with `E_NOTFOUND`. That is a state of the computer, not a fault of
 * the host: the stream goes on, without sound, until a device appears. The Windows backend wakes the wait from
 * `IMMNotificationClient` (a device was added, became active, or became the default); a poll covers a missed notification.
 */
#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace audio_default_wait {
  /**
   * @brief Lets a reason through once per session.
   *
   * The audio code asks for the default device again every few seconds while it waits, and each answer used to be an error line:
   * one machine wrote hundreds. `first("reason")` is true the first time it sees a reason after construction or `reset()`.
   */
  class once_per_session_t {
  public:
    /// True only for the first call with this reason in the current session.
    bool
    first(std::string_view reason) {
      std::lock_guard lock { mutex_ };
      return seen_.emplace(reason).second;
    }

    /// A new session: every reason may be said again.
    void
    reset() {
      std::lock_guard lock { mutex_ };
      seen_.clear();
    }

  private:
    std::mutex mutex_;
    std::set<std::string, std::less<>> seen_;
  };

  /// A wake-up that can be raised from a notification thread and waited on with a timeout; a signal raised while nobody waits is kept.
  class gate_t {
  public:
    void
    signal() {
      {
        std::lock_guard lock { mutex_ };
        raised_ = true;
      }
      condition_.notify_all();
    }

    /// Waits for a signal or the timeout; consumes the signal. True when it was signalled.
    bool
    wait_for(std::chrono::milliseconds timeout) {
      std::unique_lock lock { mutex_ };
      const bool signalled = condition_.wait_for(lock, timeout, [this] { return raised_; });
      raised_ = false;
      return signalled;
    }

  private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool raised_ = false;
  };

  /**
   * @brief Waits until `present()` is true or `stop()` is.
   * @param present Whether a default playback device exists now.
   * @param stop Whether the stream ended; checked before every wait.
   * @param wait Blocks up to the given time, ends early when something changed (a gate's `wait_for`).
   * @param poll The longest time between two looks at `present()` when no notification arrives.
   * @return True when a device is present, false when `stop()` ended the wait first.
   */
  inline bool
  wait_until_present(const std::function<bool()> &present,
    const std::function<bool()> &stop,
    const std::function<void(std::chrono::milliseconds)> &wait,
    std::chrono::milliseconds poll = std::chrono::milliseconds { 1000 }) {
    while (!stop()) {
      if (present()) {
        return true;
      }
      wait(poll);
    }
    return false;
  }

  /**
   * @brief Waits until the control reports a default device and its sink information can be read.
   * @tparam Control Anything with `default_output_missing()`, `sink_info()` (an optional of the sink data) and
   *         `wait_for_default_output(std::chrono::milliseconds)`: platf::audio_control_t, or a fake in a test.
   * @return The sink information, or an empty optional when `stop()` ended the wait first.
   */
  template<class Control>
  auto
  wait_for_sink(Control &control, const std::function<bool()> &stop, std::chrono::milliseconds poll = std::chrono::milliseconds { 1000 })
    -> decltype(control.sink_info()) {
    decltype(control.sink_info()) found;
    wait_until_present(
      [&]() {
        if (control.default_output_missing()) {
          return false;
        }
        found = control.sink_info();
        return found.has_value();
      },
      stop,
      [&](std::chrono::milliseconds timeout) { control.wait_for_default_output(timeout); },
      poll);
    return found;
  }

  /// The reasons a state of the system has, as stable words: the log line, the diagnostics page and the API use the same ones.
  namespace reason {
    constexpr std::string_view no_endpoint = "no_endpoint";  ///< no playback device exists at all
    constexpr std::string_view none_active = "none_active";  ///< devices exist, all disabled or unplugged
    constexpr std::string_view no_default = "no_default";  ///< active devices exist but none is the default
    constexpr std::string_view service = "audio_service";  ///< the audio service did not answer
  }  // namespace reason

  /**
   * @brief Why there is no default playback device.
   * @param render_total playback endpoints that are active, disabled or unplugged
   * @param render_active playback endpoints that are active
   * @param hresult_not_found whether the system said "not found" (E_NOTFOUND) as opposed to another failure
   */
  inline std::string_view
  classify(int render_total, int render_active, bool hresult_not_found) {
    if (!hresult_not_found) {
      return reason::service;
    }
    if (render_total <= 0) {
      return reason::no_endpoint;
    }
    return render_active <= 0 ? reason::none_active : reason::no_default;
  }
}  // namespace audio_default_wait
