/**
 * @file src/input_activity.h
 * @brief Declarations for input activity detection used by the VRR input activity boost.
 */
#pragma once

struct _NV_INPUT_HEADER;

namespace input::activity {

  /**
   * @brief Decides whether an incoming input packet represents meaningful user activity.
   * @details Tracks keyboard input, mouse buttons and scrolling. Pointer motion
   *          only counts while local cursor rendering is active, because video
   *          cursor motion already produces capture updates. Zero-delta motion
   *          and scrolling packets are ignored.
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
  };

}  // namespace input::activity
