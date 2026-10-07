/**
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 Foundation Sunshine contributors
 *
 * @file src/platform/windows/pyrowave/cpu_encoder.h
 * @brief Opt-in Sunshine PyroWave encoder session for system-memory capture.
 */
#pragma once

#include "src/video.h"

#include <memory>

namespace platf::pyrowave_windows {
  std::unique_ptr<video::encode_session_t>
  make_encoder(
    int width,
    int height,
    std::size_t packet_boundary,
    bool full_range,
    int dynamic_range,
    int bitrate_kbps,
    int frame_rate_num,
    int frame_rate_den,
    std::uint32_t perf_session_id = 0);

  int
  encode_frame(
    int64_t frame_number,
    video::encode_session_t &session,
    safe::mail_raw_t::queue_t<video::packet_t> &packets,
    void *channel_data,
    std::optional<std::chrono::steady_clock::time_point> frame_timestamp,
    std::optional<platf::frame_pipeline_trace_t> pipeline_trace);
}  // namespace platf::pyrowave_windows
