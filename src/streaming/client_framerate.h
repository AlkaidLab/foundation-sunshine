/**
 * @file src/streaming/client_framerate.h
 * @brief The frame rate a stream starts with when the client's request cannot be used as sent.
 *
 * A client announces its rate in `x-nv-video[0].maxFPS`. The value is divided by in the capture pacing
 * (display_base_t::capture works with numerator and denominator of the rate), so 0 or a negative number must never reach it:
 * 11-19 crash reports of one install, six times in a row at the first pacing group, EXCEPTION_INT_DIVIDE_BY_ZERO.
 */
#pragma once

namespace streaming {

  /// The rate used when a client announces none (0 or less), the usual default of the stream.
  constexpr int default_client_framerate = 60;

  /// The highest rate the host takes from an announcement; a larger number is a broken client, not a display.
  constexpr int max_client_framerate = 1000;

  /// The rate to stream at for an announced `requested` value: itself when usable, otherwise the default or the highest rate.
  constexpr int
  usable_client_framerate(int requested) {
    if (requested <= 0) {
      return default_client_framerate;
    }
    return requested > max_client_framerate ? max_client_framerate : requested;
  }

}  // namespace streaming
