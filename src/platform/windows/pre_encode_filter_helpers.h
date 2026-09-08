/** @file src/platform/windows/pre_encode_filter_helpers.h
 * @brief Shared D3D11 frame validation and result construction.
 */
#pragma once
#include "pre_encode_filter.h"

namespace platf::dxgi::filter_detail {
  template <class T>
  struct com_release_t {
    void
    operator()(T *value) const {
      if (value) {
        value->Release();
      }
    }
  };

  template <class T>
  using com_ptr_t = std::unique_ptr<T, com_release_t<T>>;

  inline std::string_view
  validate_sdr_input(const gpu_frame_view_t &input) {
    if (!input.texture || !input.srv || input.width == 0 || input.height == 0) {
      return "invalid_input";
    }
    if (input.semantic.domain != frame_domain_e::sdr_rec709 ||
        input.semantic.encoding != pixel_encoding_class_e::unorm8 ||
        input.semantic.borrowed) {
      return "input_contract_mismatch";
    }
    // X8 is bit-identical to BGRA8 except for a padding alpha channel, which
    // neither the mock shader nor the vendor backend reads. The frame always
    // arrives on this filter's own device via the private handoff copy, so
    // there is no adapter identity to validate here.
    if (input.format != DXGI_FORMAT_B8G8R8A8_UNORM &&
        input.format != DXGI_FORMAT_R8G8B8A8_UNORM &&
        input.format != DXGI_FORMAT_B8G8R8X8_UNORM) {
      return "unsupported_format";
    }
    return {};
  }

  inline filter_result_t
  make_scrgb_result(
    const gpu_frame_view_t &input,
    ID3D11Texture2D *texture,
    ID3D11ShaderResourceView *srv) {
    auto output_semantic = input.semantic;
    output_semantic.domain = frame_domain_e::linear_scrgb;
    output_semantic.encoding = pixel_encoding_class_e::float16;
    output_semantic.reference_white_nits = 80.0f;
    output_semantic.borrowed = false;
    return {
      .status = filter_status_e::ready,
      .frame = {
        .texture = texture,
        .srv = srv,
        .format = DXGI_FORMAT_R16G16B16A16_FLOAT,
        .semantic = output_semantic,
        .width = input.width,
        .height = input.height,
      },
      .reason = {},
    };
  }

}  // namespace platf::dxgi::filter_detail
