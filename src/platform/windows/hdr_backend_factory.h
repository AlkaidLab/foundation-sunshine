/** @file src/platform/windows/hdr_backend_factory.h
 * @brief Dispatch registered vendor enhancement implementations without exposing vendor SDK types.
 */
#pragma once
#include "pre_encode_filter.h"

#include <string_view>

namespace platf::dxgi {
  std::unique_ptr<pre_encode_filter_t>
  make_hdr_backend(
    std::string_view id, ID3D11Device *device, ID3D11DeviceContext *context,
    const std::filesystem::path &path, const pre_encode_filter_config_t &config,
    std::string_view runtime_digest, std::string &error);
}
