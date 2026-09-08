/**
 * @file src/platform/windows/frame_contract.cpp
 * @brief D3D11 mappings for vendor-neutral frame contracts.
 */

#include "frame_contract.h"

namespace platf::dxgi {
  DXGI_FORMAT
  select_wgc_capture_format(const capture_contract_t &contract) {
    if (contract.preferred_encoding == pixel_encoding_class_e::float16 ||
        (contract.preferred_encoding == pixel_encoding_class_e::automatic &&
         contract.required_domain == frame_domain_e::linear_scrgb)) {
      return DXGI_FORMAT_R16G16B16A16_FLOAT;
    }
    return DXGI_FORMAT_B8G8R8A8_UNORM;
  }

  captured_frame_desc_t
  describe_dxgi_captured_frame(
    DXGI_FORMAT format,
    bool linear_gamma,
    bool borrowed,
    std::uint64_t adapter_luid,
    std::uint64_t source_generation) {
    captured_frame_desc_t desc {
      .reference_white_nits = 80.0f,
      .adapter_luid = adapter_luid,
      .borrowed = borrowed,
      .source_generation = source_generation,
    };

    switch (format) {
      case DXGI_FORMAT_B8G8R8A8_UNORM:
      case DXGI_FORMAT_B8G8R8X8_UNORM:
      case DXGI_FORMAT_R8G8B8A8_UNORM:
        desc.domain = frame_domain_e::sdr_rec709;
        desc.encoding = pixel_encoding_class_e::unorm8;
        break;
      case DXGI_FORMAT_R16G16B16A16_FLOAT:
        desc.domain = linear_gamma ? frame_domain_e::linear_scrgb : frame_domain_e::unknown;
        desc.encoding = pixel_encoding_class_e::float16;
        break;
      default:
        desc.reference_white_nits = 0.0f;
        break;
    }

    return desc;
  }
}  // namespace platf::dxgi
