/**
 * @file src/ds5/pad_test.h
 * @brief Tests of the controller of a stream that the Web UI can start on purpose: identify the pad, feel a trigger.
 *
 * Like the rumble test (rumble_test.h) they go through the feedback queue a game's own output uses, so the stream
 * carries them to the client's pad: `POST /api/dualsense/pad-test` with {"action": "identify" | "trigger_left" |
 * "trigger_right" | "stop"}.  They are tests and nothing else: fixed effects, one at a time, nothing is read or
 * changed in a setting, and what the pad had before (the light bar colour, the trigger effect a game holds) is given
 * back afterwards.  Whether the client's pad did it is not known to the host: the answer says "sent".
 *
 *   identify       the light bar flashes white three times (250 ms on, 150 ms off), then shows what it showed before
 *   trigger_left   a moderate resistance on the left trigger for 1.5 s, then the trigger is released
 *   trigger_right  the same on the right trigger
 *   stop           everything above ends at once: light bar, both triggers and a running rumble pulse; a test that is
 *                  waiting ends its effect at the next 25 ms and answers as usual
 */
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string_view>

namespace ds5::pad_test {
  enum class action_e {
    identify,
    trigger_left,
    trigger_right,
    stop,
  };

  /** One thing a pad is asked to do; the sequence of an action is made of these. */
  enum class step_e {
    lightbar_white,
    lightbar_off,
    lightbar_restore,  ///< the colour the pad showed before the test
    trigger_left_on,
    trigger_left_off,  ///< release, then what a game holds on that trigger
    trigger_right_on,
    trigger_right_off,
    stop_all,
  };

  enum class outcome_e {
    sent,  ///< the test is in the feedback queue of at least one live pad
    no_pad,  ///< no virtual pad lives (no stream, or the stream has no pad)
    unsupported,  ///< this build has no native pad path
    busy,  ///< another test is running
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

  inline std::optional<action_e>
  parse_action(std::string_view name) noexcept {
    if (name == "identify") return action_e::identify;
    if (name == "trigger_left") return action_e::trigger_left;
    if (name == "trigger_right") return action_e::trigger_right;
    if (name == "stop") return action_e::stop;
    return std::nullopt;
  }

  struct result_t {
    outcome_e outcome { outcome_e::unsupported };
    unsigned pads {};  ///< live pads the test was handed to
  };

  /** The effect on a trigger: ten zones, from the third on, strength 5 of 8 (mode 0x21, the multi-position feedback block). */
  inline std::array<std::uint8_t, 11>
  trigger_test_block() noexcept {
    std::uint32_t packed = 0;
    std::uint16_t mask = 0;
    for (std::uint32_t zone = 2; zone < 10; ++zone) {
      mask = static_cast<std::uint16_t>(mask | (1u << zone));
      packed |= 4u << (3u * zone);  // stored 4 = strength 5
    }
    return { 0x21, static_cast<std::uint8_t>(mask), static_cast<std::uint8_t>(mask >> 8), static_cast<std::uint8_t>(packed),
      static_cast<std::uint8_t>(packed >> 8), static_cast<std::uint8_t>(packed >> 16), static_cast<std::uint8_t>(packed >> 24), 0, 0, 0, 0 };
  }

  inline constexpr std::chrono::milliseconds flash_on { 250 };
  inline constexpr std::chrono::milliseconds flash_off { 150 };
  inline constexpr unsigned flashes = 3;
  inline constexpr std::chrono::milliseconds trigger_hold { 1500 };

  /** Handed to every live pad: one step; returns the pads reached. */
  using handler_t = unsigned (*)(step_e step);

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

  /** Set by "stop"; the test that is running ends its effect early. */
  inline std::atomic_bool &
  stop_requested() {
    static std::atomic_bool value { false };
    return value;
  }

  inline constexpr std::chrono::milliseconds wait_slice { 25 };

  /** One test through the registered handler; `wait` sleeps (replaced in tests). */
  template<class Wait>
  result_t
  run(action_e action, Wait &&wait) {
    const auto native = handler().load(std::memory_order_acquire);
    if (native == nullptr) return { outcome_e::unsupported, 0 };
    if (action == action_e::stop) {
      /* never waits for a test that is running: it asks that one to end and ends everything itself */
      stop_requested().store(true, std::memory_order_release);
      const auto pads = native(step_e::stop_all);
      return pads == 0 ? result_t { outcome_e::no_pad, 0 } : result_t { outcome_e::sent, pads };
    }
    std::unique_lock lock(one_at_a_time(), std::try_to_lock);
    if (!lock.owns_lock()) return { outcome_e::busy, 0 };
    stop_requested().store(false, std::memory_order_release);
    /* a pause in slices, so that "stop" is heard within 25 ms */
    const auto pause = [&](std::chrono::milliseconds total) {
      for (std::chrono::milliseconds waited { 0 }; waited < total && !stop_requested().load(std::memory_order_acquire); waited += wait_slice) {
        wait(std::min(wait_slice, total - waited));
      }
    };
    unsigned pads = 0;
    switch (action) {
      case action_e::identify:
        for (unsigned flash = 0; flash < flashes && !stop_requested().load(std::memory_order_acquire); ++flash) {
          pads = native(step_e::lightbar_white);
          if (pads == 0) return { outcome_e::no_pad, 0 };
          pause(flash_on);
          native(step_e::lightbar_off);
          pause(flash_off);
        }
        native(step_e::lightbar_restore);
        break;
      case action_e::trigger_left:
      case action_e::trigger_right: {
        const bool left = action == action_e::trigger_left;
        pads = native(left ? step_e::trigger_left_on : step_e::trigger_right_on);
        if (pads == 0) return { outcome_e::no_pad, 0 };
        pause(trigger_hold);
        native(left ? step_e::trigger_left_off : step_e::trigger_right_off);
        break;
      }
      case action_e::stop:
        break;
    }
    return { outcome_e::sent, pads };
  }
}  // namespace ds5::pad_test
