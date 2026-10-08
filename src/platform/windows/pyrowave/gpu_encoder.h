/**
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 Foundation Sunshine contributors
 *
 * @file src/platform/windows/pyrowave/gpu_encoder.h
 * @brief Opt-in PyroWave encoder session for the D3D11 capture path.
 */
#pragma once

#include "src/video.h"

#include <memory>
#include <optional>

namespace platf {
  struct img_t;
}

namespace platf::dxgi {
  class display_vram_t;
}

namespace platf::pyrowave_windows {

  [[nodiscard]] std::unique_ptr<video::encode_session_t>
  make_gpu_encoder(
    std::shared_ptr<dxgi::display_vram_t> display,
    const video::config_t &config,
    std::size_t packet_boundary,
    int frame_rate_num,
    int frame_rate_den);

  [[nodiscard]] bool
  is_gpu_encoder_session(const video::encode_session_t &session) noexcept;

  int
  encode_gpu_frame(
    int64_t frame_number,
    video::encode_session_t &session,
    safe::mail_raw_t::queue_t<video::packet_t> &packets,
    void *channel_data,
    std::optional<std::chrono::steady_clock::time_point> frame_timestamp,
    std::optional<platf::frame_pipeline_trace_t> pipeline_trace);

}  // namespace platf::pyrowave_windows
