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

  /// A frame rate as the numerator/denominator pair the encoder and the capture pacing consume.
  struct frame_rate_rational_t {
    int num;
    int den;

    constexpr bool
    operator==(const frame_rate_rational_t &) const = default;
  };

  /// The rate to stream at when the client also announces `x-nv-video[0].clientRefreshRateX100`
  /// (its display refresh rate times 100, e.g. 5994 for 59.94): the fractional rate when it refines
  /// `effective_framerate` (within 2% below it, never above), otherwise the integer rate.
  ///
  /// `effective_framerate` is normalized through usable_client_framerate() first, so the fractional
  /// path cannot stream faster than the fallback or the cap that replaced an unusable announcement:
  /// the same bound binds both paths, all the way into frameRateNum/frameRateDen.
  constexpr frame_rate_rational_t
  effective_frame_rate(int client_refresh_rate_x100, int effective_framerate) {
    effective_framerate = usable_client_framerate(effective_framerate);
    if (client_refresh_rate_x100 > 0) {
      const double ratio = client_refresh_rate_x100 / 100.0 / effective_framerate;
      if (ratio > 0.98 && ratio <= 1.0) {
        const int remainder = client_refresh_rate_x100 % 100;
        if (remainder == 0) {
          return { client_refresh_rate_x100 / 100, 1 };
        }
        return { client_refresh_rate_x100, 100 };
      }
    }
    return { effective_framerate, 1 };
  }

}  // namespace streaming
