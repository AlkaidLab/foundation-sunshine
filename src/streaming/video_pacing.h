/**
 * @file src/streaming/video_pacing.h
 * @brief Rate selection and packet time slots for large video frames.
 *
 * Pacing exists to keep one frame from leaving as a single line-rate burst
 * (RFC 8085 section 3.1.6, fs-rfc8085-udp-20261112). It must never become a
 * queue of its own: the video thread sends frames one after another, so a
 * frame that takes longer to pace than the frame interval delays every frame
 * behind it, and a full packet queue is cleared as a whole (safe::queue_t).
 * WebRTC's pacer keeps the same two guards: the pacing rate is the media rate
 * times a multiplier above one, and a queue that grows past its time limit is
 * drained at the rate that meets the limit (fs-webrtc-pacing-controller-h-20261115,
 * fs-webrtc-pacing-controller-cc-20261115, fs-webrtc-goog-cc-20261115).
 * Here the limit is a share of the frame interval, because a game stream has
 * no use for a frame that arrives later than the next one.
 */
#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace streaming {

  constexpr std::uint64_t video_pacing_headroom_percent = 80;

  /// A paced frame is spread over at most this share of the frame interval, whatever its size.
  constexpr std::uint64_t video_pacing_frame_share_percent = 50;

  /**
   * @brief The sustained pacing rate: what a frame of ordinary size is paced at.
   *
   * A measurement above the encoder budget is a lower bound on the path, so
   * it is used as the path rate. A lower measurement is this process's own
   * recent output (the meter divides by the whole window) and pacing from it
   * ratchets down. Otherwise pace at the configured rate with headroom.
   */
  inline std::uint64_t
  video_pacing_rate_bps(int measured_kbps, int configured_kbps) noexcept {
    if (measured_kbps > configured_kbps && measured_kbps > 0) {
      return static_cast<std::uint64_t>(measured_kbps) * 1000;
    }
    if (configured_kbps <= 0) return 0;
    return static_cast<std::uint64_t>(configured_kbps) * 1000 * video_pacing_headroom_percent / 100;
  }

  /**
   * @brief The rate one frame is paced at: the sustained rate, raised so the frame fits its share of the frame interval.
   *
   * Without the raise, a frame at the average size needs 1 / headroom of the frame interval and a larger one more, so
   * the pacer falls behind the capture clock exactly when the picture changes most.
   *
   * @param sustained_bps The rate from video_pacing_rate_bps().
   * @param wire_bytes The bytes this frame puts on the wire, parity included.
   * @param frame_interval The capture interval of the stream.
   * @return 0 when the frame cannot be paced (no sustained rate or no interval).
   */
  inline std::uint64_t
  video_pacing_frame_rate_bps(std::uint64_t sustained_bps, std::size_t wire_bytes, std::chrono::nanoseconds frame_interval) noexcept {
    if (sustained_bps == 0 || frame_interval.count() <= 0) return 0;
    const long double budget_seconds = static_cast<long double>(frame_interval.count()) * 1e-9L *
                                       static_cast<long double>(video_pacing_frame_share_percent) / 100.0L;
    const auto needed = static_cast<long double>(wire_bytes) * 8.0L / budget_seconds;
    return std::max(sustained_bps, static_cast<std::uint64_t>(needed + 0.5L));
  }

  /// The frame rates a client may ask for while a stream runs (control message and HTTP API).
  inline constexpr double dynamic_fps_min = 1.0;
  inline constexpr double dynamic_fps_max = 1000.0;

  /// True for a finite frame rate inside [dynamic_fps_min, dynamic_fps_max]; NaN and infinity are out.
  inline bool
  dynamic_fps_in_range(double frames_per_second) noexcept {
    return frames_per_second >= dynamic_fps_min && frames_per_second <= dynamic_fps_max;
  }

  /// The capture interval of a stream at the given frame rate; zero when the rate is unusable, which disables pacing.
  inline std::chrono::nanoseconds
  video_frame_interval(double frames_per_second) noexcept {
    if (!(frames_per_second > 0.0)) return {};
    const double interval_ns = 1e9 / frames_per_second;
    // A rate so small that the interval no longer fits the counter would be undefined behaviour in the conversion.
    if (!(interval_ns < 9.0e18)) return {};
    return std::chrono::nanoseconds { static_cast<std::int64_t>(interval_ns) };
  }

  /**
   * @brief The bytes a frame puts on the wire, from its planned FEC layout (video_fec::frame_t).
   *
   * The layout is the source of truth: a client's minimum parity packet count can plan far more parity than the
   * configured percentage, and every shard is a full packet.
   */
  template <class Layout>
  inline std::size_t
  video_pacing_wire_bytes(const Layout &layout, std::size_t packet_bytes) noexcept {
    std::size_t packets = 0;
    for (std::size_t index = 0; index < layout.count; ++index) {
      packets += static_cast<std::size_t>(layout.blocks[index].data) + static_cast<std::size_t>(layout.blocks[index].parity);
    }
    return packets * packet_bytes;
  }

  inline bool
  should_pace_video_frame(bool key_frame, std::size_t frame_bytes, std::size_t burst_budget_bytes) noexcept {
    (void) key_frame;
    return burst_budget_bytes != 0 && frame_bytes >= burst_budget_bytes;
  }

  // fs-rfc8085-udp-20261112 section 3.1.6 recommends regulating UDP bursts. A slot is the
  // elapsed time at which packet_index may be submitted at rate_bps.
  //
  // The result is floor(packet_index * packet_bytes * 8e9 / rate_bps) in whole nanoseconds, computed in integers so
  // the value is exact and the same on every compiler. It used to go through long double, which on x86 means x87:
  // Clang switches the x87 rounding mode around every float-to-integer conversion, and that made this function
  // 3x slower than under GCC. Every frame a stream sends (well under 2 GB on the wire) takes one 64-bit multiply and
  // one 64-bit divide; a divide estimated through a double was slower than the divide itself on a 13th-generation Core
  // under both compilers, and Clang's build of it was 40% slower than GCC's. Larger inputs use 128-bit integers where
  // the compiler has them, and the result saturates at the largest nanosecond count instead of wrapping.
  inline std::chrono::nanoseconds
  video_packet_slot(std::size_t packet_index, std::size_t packet_bytes, std::uint64_t rate_bps) noexcept {
    if (rate_bps == 0 || packet_bytes == 0 || packet_index == 0) return {};
    constexpr std::uint64_t ns_per_byte_second = 8ull * 1'000'000'000ull;
    constexpr auto max_ns = static_cast<std::uint64_t>(INT64_MAX);
#if defined(__SIZEOF_INT128__) && !defined(SUNSHINE_VIDEO_PACING_NO_INT128)
    const auto bytes = static_cast<unsigned __int128>(packet_index) * packet_bytes;
    if (bytes <= UINT64_MAX / ns_per_byte_second) {
      return std::chrono::nanoseconds { static_cast<std::int64_t>(std::min(static_cast<std::uint64_t>(bytes) * ns_per_byte_second / rate_bps, max_ns)) };
    }
    // floor((q * rate + r) * k / rate) = q * k + floor(r * k / rate), with r < rate <= 2^64 so r * k fits in 128 bits.
    const auto whole = bytes / rate_bps;
    const auto rest = bytes % rate_bps;
    if (whole > max_ns / ns_per_byte_second) return std::chrono::nanoseconds { INT64_MAX };
    const auto ns = static_cast<std::uint64_t>(whole) * ns_per_byte_second + static_cast<std::uint64_t>(rest * ns_per_byte_second / rate_bps);
    return std::chrono::nanoseconds { static_cast<std::int64_t>(std::min(ns, max_ns)) };
#else
    // No 128-bit integer (32-bit targets, MSVC). The 64-bit path is exact while the product below fits in 64 bits,
    // which holds for every frame a stream sends; beyond it a double is accurate to far under a nanosecond per slot.
    if (static_cast<std::uint64_t>(packet_index) <= UINT64_MAX / ns_per_byte_second / packet_bytes) {
      const auto ns = static_cast<std::uint64_t>(packet_index) * packet_bytes * ns_per_byte_second / rate_bps;
      return std::chrono::nanoseconds { static_cast<std::int64_t>(std::min(ns, max_ns)) };
    }
    const double ns = static_cast<double>(packet_index) * static_cast<double>(packet_bytes) * 8e9 / static_cast<double>(rate_bps);
    return std::chrono::nanoseconds { ns >= 9.2e18 ? INT64_MAX : static_cast<std::int64_t>(ns) };
#endif
  }

  /**
   * @brief The schedule of the video send thread: where each paced frame starts and when each batch is due.
   *
   * A frame is paced when it is large enough, the stream has a frame interval, and no frame is waiting behind it. A
   * waiting frame means the thread is already late, and pacing would only make it later, so that frame goes out at
   * once and the thread catches up. The schedule a frame leaves is never later than one frame interval after it began,
   * so it cannot reach into the next capture.
   */
  class video_pacer_t {
  public:
    using clock = std::chrono::steady_clock;

    struct frame_t {
      bool paced = false;
      clock::time_point start {};
      std::uint64_t rate_bps = 0;
      std::chrono::nanoseconds interval {};
    };

    explicit video_pacer_t(clock::time_point now = clock::now()):
        next_frame_start_ { now } {}

    frame_t
    begin_frame(
      clock::time_point now,
      bool frame_waiting,
      bool large_enough,
      std::uint64_t sustained_bps,
      std::size_t wire_bytes,
      std::chrono::nanoseconds frame_interval) const noexcept {
      frame_t frame;
      frame.start = now;
      frame.interval = frame_interval;
      frame.rate_bps = video_pacing_frame_rate_bps(sustained_bps, wire_bytes, frame_interval);
      frame.paced = large_enough && !frame_waiting && frame.rate_bps != 0;
      if (frame.paced) frame.start = std::max(next_frame_start_, now);
      return frame;
    }

    /// The time batch number packets_sent (counted in packets already submitted) may go out.
    [[nodiscard]] clock::time_point
    due(const frame_t &frame, std::size_t packets_sent, std::size_t packet_bytes) const noexcept {
      return frame.start + video_packet_slot(packets_sent, packet_bytes, frame.rate_bps);
    }

    void
    end_frame(const frame_t &frame, std::size_t packets_sent, std::size_t packet_bytes, clock::time_point now) noexcept {
      if (!frame.paced) {
        next_frame_start_ = now;
        return;
      }
      next_frame_start_ = std::min(frame.start + video_packet_slot(packets_sent, packet_bytes, frame.rate_bps), frame.start + frame.interval);
    }

    [[nodiscard]] clock::time_point
    next_frame_start() const noexcept {
      return next_frame_start_;
    }

  private:
    clock::time_point next_frame_start_;
  };

}  // namespace streaming
