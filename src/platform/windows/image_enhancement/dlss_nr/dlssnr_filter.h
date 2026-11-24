/**
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 Foundation Sunshine contributors
 *
 * @file src/platform/windows/image_enhancement/dlss_nr/dlssnr_filter.h
 * @brief NVIDIA DLSS NR adapter for the neutral pre-encode filter contract.
 */
#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "src/platform/windows/pre_encode_filter.h"

namespace platf::dxgi::image_enhancement::dlss_nr {
  /**
   * Load the DLSS NR adapter component and return the SDR-to-SDR neural
   * filter. Returns null with `error` set when the component is missing or
   * untrusted; the caller is expected to fall back to the neutral identity
   * passthrough. `runtime_digest` is the pinned digest from the persisted
   * component settings; empty means the runtime is accepted unpinned.
   */
  std::unique_ptr<pre_encode_filter_t>
  make_filter(ID3D11Device *device, ID3D11DeviceContext *context,
    const std::filesystem::path &path, const pre_encode_filter_config_t &config,
    std::string_view runtime_digest, std::string &error);
}
