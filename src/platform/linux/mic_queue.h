/**
 * @file src/platform/linux/mic_queue.h
 * @brief Bounded hand-off queue between the shared microphone mixer and the
 *        blocking PulseAudio "simple" playback stream.
 *
 * The Windows backend never blocks the shared microphone thread: it asks the
 * capture endpoint how much buffer space is free and reports a dropped frame
 * (returns 0) when there is none. pa_simple_write() has no such query and
 * blocks instead, which would stall the mixer - and with it the microphone of
 * every session - on a wedged sink, and would also delay the mix timer.
 *
 * This queue decouples the two halves: write_mic_pcm() only enqueues (a full
 * queue is exactly the documented backpressure drop), while a dedicated writer
 * thread performs the blocking write. The capacity doubles as the maximum
 * staging latency; the default mirrors the 100 ms endpoint buffer the Windows
 * backend requests.
 */
#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace platf::mic_queue {
  class queue_t {
  public:
    /// 5 frames x 20 ms = the 100 ms buffer the Windows backend asks for.
    static constexpr std::size_t default_capacity_frames = 5;

    explicit queue_t(std::size_t capacity_frames = default_capacity_frames):
      capacity_frames_ { capacity_frames } {
    }

    queue_t(const queue_t &) = delete;
    queue_t &operator=(const queue_t &) = delete;

    /**
     * @brief Stage one frame for the writer thread.
     * @returns false when the queue is full, i.e. the sink is behind and the
     *          caller must report a dropped frame.
     */
    bool
    push(const std::int16_t *samples, std::size_t frame_count) {
      if (!samples || frame_count == 0) {
        // Nothing to stage; an empty frame is not a backpressure condition.
        return true;
      }

      std::lock_guard lock { mutex_ };
      if (stopped_ || frames_.size() >= capacity_frames_) {
        return false;
      }

      frames_.emplace_back(samples, samples + frame_count);
      cond_.notify_one();
      return true;
    }

    /**
     * @brief Take the oldest frame, waiting for one to arrive.
     * @returns false once stop() was called (pending frames are discarded).
     */
    bool
    pop(std::vector<std::int16_t> &out) {
      std::unique_lock lock { mutex_ };
      cond_.wait(lock, [this] { return stopped_ || !frames_.empty(); });
      if (stopped_ || frames_.empty()) {
        return false;
      }

      out = std::move(frames_.front());
      frames_.pop_front();
      return true;
    }

    /// Discard pending frames and wake the consumer so it can exit.
    void
    stop() {
      {
        std::lock_guard lock { mutex_ };
        stopped_ = true;
        frames_.clear();
      }
      cond_.notify_all();
    }

    /// Re-arm after stop(), for a device that is being (re)initialized.
    void
    reset() {
      std::lock_guard lock { mutex_ };
      frames_.clear();
      stopped_ = false;
    }

    std::size_t
    size() const {
      std::lock_guard lock { mutex_ };
      return frames_.size();
    }

    std::size_t
    capacity() const {
      return capacity_frames_;
    }

  private:
    const std::size_t capacity_frames_;
    mutable std::mutex mutex_;
    std::condition_variable cond_;
    std::deque<std::vector<std::int16_t>> frames_;
    bool stopped_ = false;
  };
}  // namespace platf::mic_queue
