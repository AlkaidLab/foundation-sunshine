/**
 * @file src/input_activity.h
 * @brief Declarations for input activity detection used by the VRR input activity boost.
 */
#pragma once

#include <array>
#include <cstdint>

struct _NV_INPUT_HEADER;
struct _NV_MULTI_CONTROLLER_PACKET;

namespace input::activity {

  /**
   * @brief Decides whether an incoming input packet represents meaningful user activity.
   * @details Filters device management/telemetry packets, zero-delta no-ops and analog
   *          jitter/drift so the video pipeline only boosts encode cadence for input
   *          that can plausibly produce visible feedback.
   */
  class tracker_t {
  public:
    /**
     * @brief Evaluate an input packet.
     * @param payload The raw input packet header.
     * @return true if the packet should count as user activity.
     */
    bool
    evaluate(_NV_INPUT_HEADER *payload);

    /**
     * @brief Reset all per-controller tracking state (e.g. on session reset).
     */
    void
    reset();

  private:
    struct controller_state_t {
      bool initialized {};
      std::uint32_t button_flags {};
      std::uint8_t left_trigger {};
      std::uint8_t right_trigger {};
      std::int16_t left_stick_x {};
      std::int16_t left_stick_y {};
      std::int16_t right_stick_x {};
      std::int16_t right_stick_y {};
    };

    bool
    evaluate_controller(_NV_MULTI_CONTROLLER_PACKET *packet);

    // activeGamepadMask is 16 bits wide, so controller numbers are bounded by 16.
    std::array<controller_state_t, 16> controllers_ {};
  };

}  // namespace input::activity
