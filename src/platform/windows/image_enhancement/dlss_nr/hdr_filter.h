/** SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 Foundation Sunshine contributors.
 */
#pragma once
#include "../../pre_encode_filter.h"

namespace platf::dxgi::image_enhancement::dlss_nr {
  // Keep captured HDR in linear FP16; the SDR-only model sees a bounded proxy.
  std::unique_ptr<pre_encode_filter_t>
  make_hdr_compatible_filter(ID3D11Device *device, ID3D11DeviceContext *context,
    std::unique_ptr<pre_encode_filter_t> model, int scale_percent = 100);
}  // namespace platf::dxgi::image_enhancement::dlss_nr
