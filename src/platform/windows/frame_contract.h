/**
 * @file src/platform/windows/frame_contract.h
 * @brief D3D11 mappings for vendor-neutral frame contracts.
 */
#pragma once

#include <dxgiformat.h>

#include "src/platform/frame_contract.h"

namespace platf::dxgi {
  DXGI_FORMAT
  select_wgc_capture_format(const capture_contract_t &contract);

  captured_frame_desc_t
  describe_dxgi_captured_frame(
    DXGI_FORMAT format,
    bool linear_gamma,
    bool borrowed,
    std::uint64_t adapter_luid,
    std::uint64_t source_generation);
}  // namespace platf::dxgi
