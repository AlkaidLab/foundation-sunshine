/**
 * @file src/ds5/mic_route.h
 * @brief The client microphone on the DualSense microphone endpoint (key 96, ds5.audio.micRoute).
 *
 * The virtual DualSense has a capture endpoint (USB Audio Class, 48 kHz, 16 bit, two channels, one isochronous IN
 * packet of 48 frames per millisecond).  It used to answer silence.  The client microphone stream that the host
 * already decodes and mixes for its virtual microphone (mic_mixer, then platf::audio::write_mic_redirect_pcm)
 * is now also handed to this endpoint:
 *
 *   mic receive thread --feed()--> ring of each live pad --serve_packet()--> isochronous IN reply
 *
 * The producer (the mic receive thread, mono 48 kHz S16, 20 ms blocks) writes with write() and never blocks.  The bus
 * clock is the consumer's clock (the transport asks for 48 frames per packet slot).  The two clocks differ, so the
 * consumer steers its read position by the fill level: inside a dead band it copies the samples unchanged, outside it
 * resamples by linear interpolation by at most 0.5 percent.  A ring that runs dry answers with silence and refills
 * to a target level before it plays again; a ring that fills beyond the high-water mark is trimmed back to the
 * target.  While no application records (the capture interface is not selected) nothing is queued, so a recording
 * never starts with old audio.  The method is the one of virtual_usb::uac1_microphone (the host's own virtual
 * microphone); it has no state shared with it.
 *
 * Pure bookkeeping on integers and atomics, tested without a device or a clock.
 */
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ds5::mic_route {
  /** Handed the mono 48 kHz S16 block of the client microphone; set by the native DualSense client. */
  using feed_t = void (*)(const std::int16_t *samples, std::size_t frames);

  inline std::atomic<feed_t> &
  handler() {
    static std::atomic<feed_t> value { nullptr };
    return value;
  }

  /** Called by the microphone redirect for every block it writes; a no-op while no DualSense exists. */
  inline void
  feed(const std::int16_t *samples, std::size_t frames) {
    if (const auto native = handler().load(std::memory_order_acquire)) native(samples, frames);
  }

  struct tuning_t {
    std::uint32_t ring_frames { 8192 };  ///< a power of two
    std::uint32_t target_frames { 960 };  ///< 20 ms: the average fill the servo steers to
    std::uint32_t start_frames { 1440 };  ///< 30 ms: playback starts at this fill (a 20 ms block arrives at once, so the fill swings by 960 around the average)
    std::uint32_t high_water_frames { 2880 };  ///< above this the consumer trims back to the target
    std::uint32_t dead_band_frames { 24 };  ///< no resampling while the fill is within this of the target
    double servo_gain { 1e-5 };  ///< ratio change per frame of fill error outside the dead band
    double max_ratio_deviation { 0.005 };
    std::chrono::milliseconds active_window { 250 };  ///< a write this recent counts as "the remote side is talking"
  };

  struct stats_t {
    std::uint64_t frames_written { 0 };
    std::uint64_t frames_idle_discarded { 0 };  ///< arrived while no application records
    std::uint64_t frames_dropped { 0 };  ///< the ring was full
    std::uint64_t frames_trimmed { 0 };  ///< discarded at the high-water mark
    std::uint64_t frames_served { 0 };
    std::uint64_t underrun_frames { 0 };  ///< silence served while the remote side was talking
    std::uint32_t underruns { 0 };  ///< distinct underrun events
    std::uint64_t packets_served { 0 };
    double ratio { 1.0 };  ///< resampling ratio of the last packet; exactly 1.0 inside the dead band
    double fill_average { 0.0 };
  };

  /** One producer thread calls write() and flush(); one consumer thread calls serve_packet(). */
  class ring_t {
  public:
    explicit ring_t(tuning_t tuning = {}):
        tuning_(tuning),
        mask_(tuning.ring_frames - 1),
        samples_(tuning.ring_frames) {}

    /**
     * Queue mono samples.  While @p capturing is false (no application records) nothing is stored.  Returns the frames
     * stored; fewer than @p frames only when the ring is full.
     */
    std::size_t
    write(const std::int16_t *samples, std::size_t frames, bool capturing, std::chrono::steady_clock::time_point now) noexcept {
      last_write_ns_.store(now.time_since_epoch().count(), std::memory_order_relaxed);
      if (!capturing) {
        idle_discarded_.fetch_add(frames, std::memory_order_relaxed);
        return 0;
      }
      const auto head = head_.load(std::memory_order_relaxed);
      const auto tail = tail_.load(std::memory_order_acquire);
      const auto room = static_cast<std::uint64_t>(tuning_.ring_frames) - (head - tail);
      const auto stored = static_cast<std::size_t>(std::min<std::uint64_t>(room, frames));
      for (std::size_t i = 0; i < stored; ++i) samples_[(head + i) & mask_] = samples[i];
      head_.store(head + stored, std::memory_order_release);
      written_.fetch_add(stored, std::memory_order_relaxed);
      if (stored < frames) dropped_.fetch_add(frames - stored, std::memory_order_relaxed);
      return stored;
    }

    /** Drop everything queued; the next stream starts from the target fill.  Any thread: the consumer does it at its next packet. */
    void
    flush() noexcept { flush_request_.store(true, std::memory_order_release); }

    /** Fill @p out (mono) with the next @p frames frames the bus asks for; always answers, with silence when it must. */
    void
    serve_packet(std::int16_t *out, std::size_t frames, std::chrono::steady_clock::time_point now) noexcept {
      const bool talking = remote_active(now);
      const bool flushing = flush_request_.exchange(false, std::memory_order_acq_rel);
      /* the head is read after the request is taken: whatever was queued before the request is dropped with it */
      auto head = head_.load(std::memory_order_acquire);
      auto tail = tail_.load(std::memory_order_relaxed);
      if (flushing) {
        tail = head;
        tail_.store(tail, std::memory_order_release);
        playing_ = false;
        underrunning_ = false;
        fraction_ = 0.0;
        fill_average_ = static_cast<double>(tuning_.target_frames);
        ratio_ = 1.0;
      }
      std::uint64_t fill = head - tail;
      bump(packets_served_, 1);
      if (!playing_) {
        if (fill >= tuning_.start_frames) {
          playing_ = true;
          /* the average the servo steers is where the fill should settle, not where it starts */
          fill_average_ = static_cast<double>(tuning_.target_frames);
          fraction_ = 0.0;
        }
        else {
          std::fill(out, out + frames, std::int16_t { 0 });
          if (talking && underrunning_) bump(underrun_frames_, frames);
          return;
        }
      }
      if (fill > tuning_.high_water_frames) {
        const auto drop = fill - tuning_.target_frames;
        tail += drop;
        tail_.store(tail, std::memory_order_release);
        bump(trimmed_, drop);
        fill = tuning_.target_frames;
      }
      fill_average_ += (static_cast<double>(fill) - fill_average_) * 0.02;
      const double error = fill_average_ - static_cast<double>(tuning_.target_frames);
      if (std::abs(error) > static_cast<double>(tuning_.dead_band_frames)) {
        ratio_ = 1.0 + std::clamp(error * tuning_.servo_gain, -tuning_.max_ratio_deviation, tuning_.max_ratio_deviation);
      }
      else {
        ratio_ = 1.0;
        fraction_ = 0.0;
      }
      const auto needed = ratio_ == 1.0 ? static_cast<std::uint64_t>(frames) : static_cast<std::uint64_t>(std::ceil(static_cast<double>(frames) * ratio_ + fraction_)) + 1;
      if (fill < needed) {
        /* ran dry: silence, drop the few stale frames that are left, and wait for the start fill again */
        std::fill(out, out + frames, std::int16_t { 0 });
        tail_.store(head, std::memory_order_release);
        playing_ = false;
        if (talking) {
          if (!underrunning_) bump(underruns_, 1);
          underrunning_ = true;
          bump(underrun_frames_, frames);
        }
        return;
      }
      underrunning_ = false;
      if (ratio_ == 1.0) {
        for (std::size_t i = 0; i < frames; ++i) out[i] = samples_[(tail + i) & mask_];
        tail += frames;
      }
      else {
        double position = fraction_;
        for (std::size_t i = 0; i < frames; ++i) {
          const auto index = static_cast<std::uint64_t>(position);
          const double part = position - static_cast<double>(index);
          const double a = samples_[(tail + index) & mask_];
          const double b = samples_[(tail + index + 1) & mask_];
          out[i] = static_cast<std::int16_t>(std::lround(a + (b - a) * part));
          position += ratio_;
        }
        const auto consumed = static_cast<std::uint64_t>(position);
        fraction_ = position - static_cast<double>(consumed);
        tail += consumed;
      }
      tail_.store(tail, std::memory_order_release);
      bump(served_, frames);
      ratio_pub_.store(ratio_, std::memory_order_relaxed);
      fill_average_pub_.store(fill_average_, std::memory_order_relaxed);
    }

    stats_t
    stats() const noexcept {
      stats_t result;
      result.frames_written = written_.load(std::memory_order_relaxed);
      result.frames_idle_discarded = idle_discarded_.load(std::memory_order_relaxed);
      result.frames_dropped = dropped_.load(std::memory_order_relaxed);
      result.frames_trimmed = trimmed_.load(std::memory_order_relaxed);
      result.frames_served = served_.load(std::memory_order_relaxed);
      result.underrun_frames = underrun_frames_.load(std::memory_order_relaxed);
      result.underruns = underruns_.load(std::memory_order_relaxed);
      result.packets_served = packets_served_.load(std::memory_order_relaxed);
      result.ratio = ratio_pub_.load(std::memory_order_relaxed);
      result.fill_average = fill_average_pub_.load(std::memory_order_relaxed);
      return result;
    }

    std::uint32_t
    buffered_frames() const noexcept {
      return static_cast<std::uint32_t>(head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire));
    }

  private:
    template<class T, class N>
    static void
    bump(std::atomic<T> &counter, N amount) noexcept {
      counter.store(static_cast<T>(counter.load(std::memory_order_relaxed) + amount), std::memory_order_relaxed);
    }

    bool
    remote_active(std::chrono::steady_clock::time_point now) const noexcept {
      const auto last = last_write_ns_.load(std::memory_order_relaxed);
      if (last == 0) return false;
      const auto age = now.time_since_epoch().count() - last;
      return age >= 0 && age <= std::chrono::duration_cast<std::chrono::steady_clock::duration>(tuning_.active_window).count();
    }

    tuning_t tuning_;
    std::uint32_t mask_;
    std::vector<std::int16_t> samples_;
    alignas(64) std::atomic<std::uint64_t> head_ { 0 };
    alignas(64) std::atomic<std::uint64_t> tail_ { 0 };
    std::atomic<std::int64_t> last_write_ns_ { 0 };
    std::atomic<std::uint64_t> written_ { 0 };
    std::atomic<std::uint64_t> idle_discarded_ { 0 };
    std::atomic<std::uint64_t> dropped_ { 0 };
    std::atomic<bool> flush_request_ { false };
    // consumer state
    bool playing_ { false };
    bool underrunning_ { false };
    double fraction_ { 0.0 };
    double ratio_ { 1.0 };
    double fill_average_ { 0.0 };
    /* counters the session thread reads while the reply thread writes them: one writer, relaxed */
    std::atomic<std::uint64_t> trimmed_ { 0 };
    std::atomic<std::uint64_t> served_ { 0 };
    std::atomic<std::uint64_t> underrun_frames_ { 0 };
    std::atomic<std::uint32_t> underruns_ { 0 };
    std::atomic<std::uint64_t> packets_served_ { 0 };
    std::atomic<double> ratio_pub_ { 1.0 };
    std::atomic<double> fill_average_pub_ { 0.0 };
  };

  /** One mono packet to the two channels of the endpoint (the same signal on both), @p muted giving silence. */
  inline void
  to_stereo(const std::int16_t *mono, std::size_t frames, bool muted, std::uint8_t *out) {
    for (std::size_t i = 0; i < frames; ++i) {
      const std::int16_t value = muted ? std::int16_t { 0 } : mono[i];
      const auto low = static_cast<std::uint8_t>(static_cast<std::uint16_t>(value) & 0xffu);
      const auto high = static_cast<std::uint8_t>(static_cast<std::uint16_t>(value) >> 8);
      out[i * 4 + 0] = low;
      out[i * 4 + 1] = high;
      out[i * 4 + 2] = low;
      out[i * 4 + 3] = high;
    }
  }
}  // namespace ds5::mic_route
