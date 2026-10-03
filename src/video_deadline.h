/**
 * @file src/video_deadline.h
 * @brief Encoder-owner history of each input frame's deadline origin.
 */
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace video {
  // No tracing or RTP timestamp state participates in this history. Repeated
  // images get their own submission origin without changing their null RTP
  // frame_timestamp. Delayed output must look up its own input identity.
  class frame_deadline_history_t {
  public:
    using clock_t = std::chrono::steady_clock;
    using time_point_t = clock_t::time_point;
    static constexpr std::size_t capacity = 256;

    void bind(std::uint64_t frame_index, std::optional<time_point_t> frame_timestamp,
              time_point_t submission_time = clock_t::now()) {
      entries_[frame_index % capacity] = {frame_index, frame_timestamp.value_or(submission_time), true};
    }

    std::optional<time_point_t> find(std::uint64_t frame_index) const {
      const auto &entry = entries_[frame_index % capacity];
      if (!entry.valid || entry.frame_index != frame_index) return std::nullopt;
      return entry.origin;
    }

  private:
    struct entry_t {
      std::uint64_t frame_index = 0;
      time_point_t origin {};
      bool valid = false;
    };
    std::array<entry_t, capacity> entries_ {};
  };
}  // namespace video
