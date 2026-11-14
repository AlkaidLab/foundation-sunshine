/**
 * @file src/streaming/measured_bitrate.h
 * @brief Lock-free sliding window over bytes actually handed to the socket.
 *
 * The hot path is add_bytes(): one compare-exchange on the current time
 * bucket, shared by the video and audio send threads, with no mutex.
 * publish/unpublish/measured_bitrate_kbps run on session start, session
 * teardown and API reads.
 *
 * The window is bucket_count buckets of bucket_ms (2 seconds). Each slot
 * stores the bucket epoch in the high 32 bits and the byte count in the
 * low 32 bits, so a reset and an add cannot tear. slot_count is twice the
 * window, so a bucket from the previous window does not share a slot with
 * the bucket that replaces it. A reader sums buckets whose epoch is still
 * inside the window and divides by the full window. No sample in the
 * window yields 0, never the previous rate.
 *
 * The epoch is the bucket index (milliseconds / bucket_ms) truncated to
 * 32 bits. That counter wraps after about 13.6 years of steady-clock
 * uptime; a host is not expected to keep one process running that long.
 */
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <mutex>
#include <vector>

namespace streaming {

  class measured_bitrate_window_t {
  public:
    static constexpr std::uint32_t bucket_ms = 100;
    static constexpr std::uint32_t window_ms = 2000;
    static constexpr std::uint32_t bucket_count = window_ms / bucket_ms;
    static constexpr std::uint32_t slot_count = bucket_count * 2;
    static constexpr std::uint32_t max_bucket_bytes = 0xffffffffu;

    measured_bitrate_window_t() = default;
    measured_bitrate_window_t(const measured_bitrate_window_t &) = delete;
    measured_bitrate_window_t &
    operator=(const measured_bitrate_window_t &) = delete;

    void
    add_bytes(std::uint64_t bytes, std::uint64_t now_ms) noexcept {
      if (bytes == 0) {
        return;
      }

      const auto epoch = static_cast<std::uint32_t>(now_ms / bucket_ms);
      auto &slot = slots_[epoch % slot_count];
      auto current = slot.load(std::memory_order_relaxed);
      for (;;) {
        const auto current_epoch = static_cast<std::uint32_t>(current >> 32);
        const auto current_bytes = static_cast<std::uint32_t>(current);
        const auto next_bytes = current_epoch == epoch ?
                                  std::min<std::uint64_t>(static_cast<std::uint64_t>(current_bytes) + bytes, max_bucket_bytes) :
                                  std::min<std::uint64_t>(bytes, max_bucket_bytes);
        const auto next = (static_cast<std::uint64_t>(epoch) << 32) | next_bytes;
        if (slot.compare_exchange_weak(current, next, std::memory_order_acq_rel, std::memory_order_relaxed)) {
          return;
        }
      }
    }

    /**
     * @brief Kilobits per second over the whole window.
     * @return 0 when no counted byte is still inside the window.
     */
    int
    kbps(std::uint64_t now_ms) const noexcept {
      const auto now_epoch = static_cast<std::uint32_t>(now_ms / bucket_ms);
      std::uint64_t total = 0;
      for (std::uint32_t index = 0; index < slot_count; ++index) {
        const auto packed = slots_[index].load(std::memory_order_acquire);
        const auto bytes = static_cast<std::uint32_t>(packed);
        if (bytes == 0) {
          continue;
        }

        const auto epoch = static_cast<std::uint32_t>(packed >> 32);
        if ((epoch % slot_count) != index) {
          continue;
        }

        // Signed age rejects a bucket that is ahead of now (clock step
        // backward, or a sample recorded in the future) without treating
        // the unsigned wrap as a small age.
        const auto age = static_cast<std::int32_t>(now_epoch - epoch);
        if (age < 0 || static_cast<std::uint32_t>(age) >= bucket_count) {
          continue;
        }
        total += bytes;
      }

      if (total == 0) {
        return 0;
      }

      const auto rate = total * 8 / window_ms;
      if (rate > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
        return std::numeric_limits<int>::max();
      }
      return static_cast<int>(rate);
    }

  private:
    std::array<std::atomic<std::uint64_t>, slot_count> slots_ {};
  };

  inline void
  publish_measured_bitrate(std::uint32_t session_id, measured_bitrate_window_t *window);
  inline void
  unpublish_measured_bitrate(std::uint32_t session_id, measured_bitrate_window_t *window);

  class measured_bitrate_handle_t {
  public:
    measured_bitrate_handle_t() = default;
    measured_bitrate_handle_t(const measured_bitrate_handle_t &) = delete;
    measured_bitrate_handle_t &
    operator=(const measured_bitrate_handle_t &) = delete;

    ~measured_bitrate_handle_t() {
      reset();
    }

    void
    publish(std::uint32_t session_id, measured_bitrate_window_t *window) {
      reset();
      if (window == nullptr) {
        return;
      }
      publish_measured_bitrate(session_id, window);
      session_id_ = session_id;
      window_ = window;
    }

    void
    reset() noexcept {
      if (window_ == nullptr) {
        return;
      }
      unpublish_measured_bitrate(session_id_, window_);
      window_ = nullptr;
    }

  private:
    std::uint32_t session_id_ = 0;
    measured_bitrate_window_t *window_ = nullptr;
  };

  namespace measured_bitrate_detail {
    struct published_t {
      std::uint32_t session_id;
      measured_bitrate_window_t *window;
    };

    struct registry_t {
      std::mutex mutex;
      std::vector<published_t> slots;
    };

    inline registry_t &
    registry() {
      static registry_t instance;
      return instance;
    }
  }  // namespace measured_bitrate_detail

  inline void
  publish_measured_bitrate(std::uint32_t session_id, measured_bitrate_window_t *window) {
    if (window == nullptr) {
      return;
    }

    auto &table = measured_bitrate_detail::registry();
    std::lock_guard lock(table.mutex);
    for (auto &slot : table.slots) {
      if (slot.session_id == session_id) {
        slot.window = window;
        return;
      }
    }
    table.slots.push_back(measured_bitrate_detail::published_t { session_id, window });
  }

  inline void
  unpublish_measured_bitrate(std::uint32_t session_id, measured_bitrate_window_t *window) {
    auto &table = measured_bitrate_detail::registry();
    std::lock_guard lock(table.mutex);
    std::erase_if(table.slots, [&](const measured_bitrate_detail::published_t &slot) {
      return slot.session_id == session_id && (window == nullptr || slot.window == window);
    });
  }

  /**
   * @brief Measured send rate for a published session, or 0 when it is not published.
   *
   * Holds the registry mutex across the window read so unpublish cannot free
   * the window first. add_bytes does not take this mutex.
   */
  inline int
  measured_bitrate_kbps(std::uint32_t session_id, std::uint64_t now_ms) {
    auto &table = measured_bitrate_detail::registry();
    std::lock_guard lock(table.mutex);
    for (const auto &slot : table.slots) {
      if (slot.session_id == session_id && slot.window != nullptr) {
        return slot.window->kbps(now_ms);
      }
    }
    return 0;
  }

}  // namespace streaming
