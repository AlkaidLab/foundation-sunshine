/**
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 Foundation Sunshine contributors
 *
 * @file src/platform/windows/hdr_enhanced/nvidia_rtx_video/truehdr_filter.h
 * @brief Factory for the NVIDIA TrueHDR provider.
 */
#pragma once
#include "../../pre_encode_filter.h"

namespace platf::dxgi::hdr_enhanced::nvidia_rtx_video::truehdr {
  std::unique_ptr<pre_encode_filter_t>
  make_filter(
    ID3D11Device *device, ID3D11DeviceContext *context, const std::filesystem::path &path,
    const pre_encode_filter_config_t &config, std::string &error);
}
