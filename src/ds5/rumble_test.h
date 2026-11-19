/**
 * @file src/ds5/rumble_test.h
 * @brief A short rumble the Web UI and the panel can send on purpose, through the path a game's rumble takes.
 *
 * The gamepad test tool of the Web UI and of the panel cannot count on the browser: WebView2 and browsers hand a pad
 * a rumble only when they list an actuator for it, and the virtual DualSense of a stream is reached from the host's own
 * window only through the USB/IP bus. `POST /api/dualsense/rumble-test` asks the native client of every live pad for one
 * short pulse in the feedback queue the game's own rumble uses, so the stream carries it to the client's pad (the same
 * gamepad_feedback_msg_t, the same saved rumble shaping), and gives the held rumble of a running game back afterwards.
 *
 * The pulse is a test and nothing else: a fixed strength, a duration of 100 to 1000 ms, one pulse at a time, no setting
 * is read or changed. Whether the client's pad moved is not known to the host: the answer says "sent".
 */
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

namespace ds5::rumble_test {
  inline constexpr unsigned default_ms = 400;
  inline constexpr unsigned min_ms = 100;
  inline constexpr unsigned max_ms = 1000;
  /** The strength of the pulse (0..255, both motors): clearly felt, nowhere near the maximum. */
  inline constexpr std::uint8_t strength = 0xC0;

  /** The duration asked for in ms, kept inside the limits; anything that is not a usable number gives the default. */
  inline unsigned
  clamp_duration(long long asked, bool given = true) noexcept {
    if (!given || asked <= 0) return default_ms;
    if (asked < static_cast<long long>(min_ms)) return min_ms;
    if (asked > static_cast<long long>(max_ms)) return max_ms;
    return static_cast<unsigned>(asked);
  }

  enum class outcome_e {
    sent,         ///< the pulse is in the feedback queue of at least one live pad
    no_pad,       ///< no virtual pad lives (no stream, or the stream has no pad)
    unsupported,  ///< this build has no native pad path
    busy,         ///< another test pulse is running
  };

  inline const char *
  to_string(outcome_e outcome) noexcept {
    switch (outcome) {
      case outcome_e::sent: return "sent";
      case outcome_e::no_pad: return "no_pad";
      case outcome_e::unsupported: return "unsupported";
      case outcome_e::busy: return "busy";
    }
    return "unsupported";
  }

  struct pulse_t {
    outcome_e outcome { outcome_e::unsupported };
    unsigned pads {};  ///< live pads the pulse was handed to
  };

  /** What a pad is given when the pulse ends: the running game's held rumble, or a stop. */
  inline std::array<std::uint8_t, 2>
  restore_values(bool game_running, const std::array<std::uint8_t, 2> &held) noexcept {
    if (game_running && (held[0] | held[1]) != 0u) return held;
    return { 0, 0 };
  }

  /** Handed to every live pad: restore false starts the pulse (left, right), restore true ends it. Returns the pads reached. */
  using handler_t = unsigned (*)(std::uint8_t left, std::uint8_t right, bool restore);

  inline std::atomic<handler_t> &
  handler() {
    static std::atomic<handler_t> value { nullptr };
    return value;
  }

  /** One mutex for every instance of run(): a static inside the template would be one per `wait` type. */
  inline std::mutex &
  one_at_a_time() {
    static std::mutex value;
    return value;
  }

  /** One pulse of `ms` through the registered handler; `wait` sleeps (replaced in tests). */
  template<class Wait>
  pulse_t
  run(unsigned ms, Wait &&wait) {
    const auto native = handler().load(std::memory_order_acquire);
    if (native == nullptr) return { outcome_e::unsupported, 0 };
    std::unique_lock lock(one_at_a_time(), std::try_to_lock);
    if (!lock.owns_lock()) return { outcome_e::busy, 0 };
    const unsigned pads = native(strength, strength, false);
    if (pads == 0) return { outcome_e::no_pad, 0 };
    wait(std::chrono::milliseconds(ms));
    native(0, 0, true);
    return { outcome_e::sent, pads };
  }
}  // namespace ds5::rumble_test
