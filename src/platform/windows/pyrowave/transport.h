#pragma once

#include "src/video.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace platf::pyrowave_windows {

  struct transport_publish_result_t {
    bool success = false;
    int result_code = 0;
    std::size_t bytes = 0;
    std::size_t blocks = 0;
  };

  [[nodiscard]] transport_publish_result_t
  publish_transport_frame(
    std::int64_t frame_number,
    std::shared_ptr<const std::vector<std::uint8_t>> bitstream,
    std::size_t packet_boundary,
    std::uint32_t rtp_timestamp,
    safe::mail_raw_t::queue_t<video::packet_t> &packets,
    void *channel_data,
    std::optional<std::chrono::steady_clock::time_point> frame_timestamp,
    std::optional<platf::frame_pipeline_trace_t> pipeline_trace) noexcept;

}  // namespace platf::pyrowave_windows
