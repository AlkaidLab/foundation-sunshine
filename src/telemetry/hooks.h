/**
 * @file src/telemetry/hooks.h
 * @brief Header-only hooks the per-frame code calls: a null pointer until telemetry::start() installs the real functions.
 *
 * The streaming code (perf_recorder) is also built into small test binaries that must not link the sender; it only includes this header.
 */
#pragma once

#include <atomic>
#include <cstdint>

namespace telemetry::hooks {

  /** Per-frame timings, milliseconds. */
  enum class metric_e : std::uint8_t {
    host_ms,  ///< capture ready to dequeued for sending (what the client shows as "host")
    capture_ms,  ///< capture ready to conversion start
    convert_ms,  ///< conversion
    encode_ms,  ///< encoder submit to packet ready
    send_ms,  ///< first to last batch of one frame, pacing waits included
    backlog_ms,  ///< packet ready to the last byte of the frame handed to the socket
    jitter_ms,  ///< interarrival jitter of the frames leaving the send thread (RFC 3550 estimator), computed by the stream tracker from frame_done()
    count_
  };

  /** Counters, added up per window. */
  enum class counter_e : std::uint8_t {
    captured,  ///< frames taken from the capture
    submitted,  ///< frames given to the encoder (repeats excluded)
    frames_sent,  ///< frames that left the send thread
    idr_requests,  ///< key frames the client asked for
    rfi_requests,  ///< reference frame invalidations the client asked for
    queue_flushes,  ///< times the packet queue was full and cleared as a whole
    queue_frames_dropped,  ///< frames lost in those clears
    send_would_block_packets,  ///< media datagrams discarded on local send backpressure
    count_
  };

  struct table_t {
    std::atomic<void (*)(std::uint32_t)> frame_encoded { nullptr };
    std::atomic<void (*)(std::uint32_t, double)> encode_ms { nullptr };
    std::atomic<void (*)(std::uint32_t, double)> capture_ms { nullptr };
    std::atomic<void (*)(std::uint32_t, metric_e, double)> sample { nullptr };
    std::atomic<void (*)(std::uint32_t, counter_e, std::uint64_t)> count { nullptr };
    std::atomic<void (*)(std::uint32_t, std::int64_t)> frame_done { nullptr };
  };

  inline table_t &
  table() {
    static table_t instance;
    return instance;
  }

  inline void
  frame_encoded(std::uint32_t session_id) noexcept {
    if (const auto fn = table().frame_encoded.load(std::memory_order_relaxed)) fn(session_id);
  }
  inline void
  encode_ms(std::uint32_t session_id, double ms) noexcept {
    if (const auto fn = table().encode_ms.load(std::memory_order_relaxed)) fn(session_id, ms);
  }
  inline void
  capture_ms(std::uint32_t session_id, double ms) noexcept {
    if (const auto fn = table().capture_ms.load(std::memory_order_relaxed)) fn(session_id, ms);
  }
  /** One per-frame timing for the stream-health windows. */
  inline void
  sample(std::uint32_t session_id, metric_e metric, double ms) noexcept {
    if (const auto fn = table().sample.load(std::memory_order_relaxed)) fn(session_id, metric, ms);
  }
  inline void
  count(std::uint32_t session_id, counter_e counter, std::uint64_t n = 1) noexcept {
    if (const auto fn = table().count.load(std::memory_order_relaxed)) fn(session_id, counter, n);
  }

  /** A frame left the send thread at `steady_ns` (steady clock, nanoseconds). */
  inline void
  frame_done(std::uint32_t session_id, std::int64_t steady_ns) noexcept {
    if (const auto fn = table().frame_done.load(std::memory_order_relaxed)) fn(session_id, steady_ns);
  }

}  // namespace telemetry::hooks
