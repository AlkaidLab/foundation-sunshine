/**
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 Foundation Sunshine contributors
 *
 * @file src/platform/windows/pyrowave/color_metadata.h
 * @brief Maps Sunshine's negotiated signal to the PyroWave wire contract.
 */
#pragma once

#include "pyrowave.h"

namespace platf::pyrowave_windows {
  [[nodiscard]] inline pyrowave_color_metadata
  color_metadata_for(int dynamic_range, bool full_range) noexcept {
    const bool hdr = dynamic_range > 0;
    return {
      .primaries = hdr ? PYROWAVE_COLOR_PRIMARIES_BT2020 : PYROWAVE_COLOR_PRIMARIES_BT709,
      .transfer = dynamic_range == 2 ? PYROWAVE_TRANSFER_HLG :
        (hdr ? PYROWAVE_TRANSFER_PQ : PYROWAVE_TRANSFER_BT709),
      .transform = hdr ? PYROWAVE_YCBCR_BT2020 : PYROWAVE_YCBCR_BT709,
      .range = full_range ? PYROWAVE_YCBCR_FULL : PYROWAVE_YCBCR_LIMITED,
      .chroma_siting = 0,
    };
  }
}  // namespace platf::pyrowave_windows
