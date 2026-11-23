#pragma once

#include "src/video.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace pyrowave {
  struct hdr_frame_metadata_t;
}

namespace platf::pyrowave_windows {

  namespace detail {
    // Count frame header, DATA, and XOR parity packets; empty means invalid
    // payload geometry or a size_t overflow in the protected length/count.
    [[nodiscard]] std::optional<std::size_t>
    transport_packet_count(std::size_t image_bytes, std::size_t metadata_bytes,
                           std::size_t payload_boundary) noexcept;
  }

  struct transport_publish_result_t {
    bool success = false;
    // This frame is too large for the outer transport; keep the session alive.
    bool dropped_oversize = false;
    bool dropped_queue = false;
    bool queue_stopped = false;
    bool replaced_pending = false;
    int result_code = 0;
    std::size_t bytes = 0;
    std::size_t blocks = 0;
  };

  // Borrowed bytes are consumed synchronously before the function returns.
  [[nodiscard]] transport_publish_result_t
  publish_transport_frame(
    std::int64_t frame_number,
    std::span<const std::uint8_t> bitstream,
    std::size_t packet_boundary,
    std::uint32_t rtp_timestamp,
    safe::mail_raw_t::queue_t<video::packet_t> &packets,
    std::uint64_t channel_data,
    std::optional<std::chrono::steady_clock::time_point> frame_timestamp,
    std::optional<platf::frame_pipeline_trace_t> pipeline_trace,
    const pyrowave::hdr_frame_metadata_t *dynamic_metadata = nullptr) noexcept;

  [[nodiscard]] transport_publish_result_t
  publish_transport_frame(
    std::int64_t frame_number,
    std::shared_ptr<const std::vector<std::uint8_t>> bitstream,
    std::size_t packet_boundary,
    std::uint32_t rtp_timestamp,
    safe::mail_raw_t::queue_t<video::packet_t> &packets,
    std::uint64_t channel_data,
    std::optional<std::chrono::steady_clock::time_point> frame_timestamp,
    std::optional<platf::frame_pipeline_trace_t> pipeline_trace,
    const pyrowave::hdr_frame_metadata_t *dynamic_metadata = nullptr) noexcept;

}  // namespace platf::pyrowave_windows
