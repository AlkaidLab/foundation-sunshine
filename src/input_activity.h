/**
 * @file src/input_activity.h
 * @brief Declarations for input activity detection used by the VRR input activity boost.
 */
#pragma once

#include <cstddef>
#include <optional>

struct _NV_INPUT_HEADER;

namespace input::activity {

  /**
   * @brief Decides whether an incoming input packet represents meaningful user activity.
   * @details Tracks keyboard input, mouse buttons and scrolling. Pointer motion
   *          only counts while local cursor rendering is active, because video
   *          cursor motion already produces capture updates. Zero-delta motion
   *          and zero-delta scrolling packets are ignored; non-zero vertical or
   *          horizontal scrolling counts as activity.
   */
  class tracker_t {
  public:
    /**
     * @brief Evaluate an input packet.
     * @param payload The raw input packet header.
     * @param payload_size The number of bytes available at payload.
     * @return No value if the packet is truncated; otherwise whether it should
     *         count as user activity.
     */
    std::optional<bool>
    evaluate(const _NV_INPUT_HEADER *payload, std::size_t payload_size);
  };

}  // namespace input::activity
