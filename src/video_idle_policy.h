/**
 * @file src/video_idle_policy.h
 * @brief When the variable-refresh-rate encode loop may stay silent.
 *
 * Header only and free of host dependencies, so the rule has its own unit test.
 */
#pragma once

#include <chrono>
#include <cstdint>

namespace video {
  /**
   * A stream that has had nothing to show for this long gets one repeated frame. A static desktop (a launcher splash
   * that does not move, a desktop nobody touches) publishes no new frame, and a client that receives no video packet
   * at all treats the connection as dead after a few seconds: black screen, then a disconnect.
   */
  inline constexpr std::chrono::milliseconds vrr_idle_keepalive { 500 };

  /**
   * @brief Whether the encode loop may skip this round (variable refresh rate, no new frame).
   *
   * Skipping is what variable refresh rate asks for: encode only when the source changed. It is never allowed for
   * the very first frame of a session, nor when the client asked for a key frame, nor when a minimum rate or an input
   * boost keeps the cadence, nor when the stream has been silent for vrr_idle_keepalive.
   *
   * @param variable_refresh_rate The host setting.
   * @param has_new_frame A captured frame is ready for this round.
   * @param requested_idr_frame The client asked for a key frame.
   * @param minimum_fps_target The host minimum rate (0 = none).
   * @param input_boost_active The input activity boost holds the cadence.
   * @param frames_encoded How many frames this session has encoded so far.
   * @param since_last_encode Time since the last encoded frame.
   */
  [[nodiscard]] constexpr bool
  vrr_may_skip_encode(
    bool variable_refresh_rate,
    bool has_new_frame,
    bool requested_idr_frame,
    int minimum_fps_target,
    bool input_boost_active,
    std::uint64_t frames_encoded,
    std::chrono::steady_clock::duration since_last_encode) {
    if (!variable_refresh_rate || has_new_frame || requested_idr_frame) {
      return false;
    }
    if (minimum_fps_target != 0 || input_boost_active) {
      return false;
    }
    if (frames_encoded == 0) {
      return false;
    }
    return since_last_encode < vrr_idle_keepalive;
  }
}  // namespace video
