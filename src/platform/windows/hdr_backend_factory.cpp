/** @file src/platform/windows/hdr_backend_factory.cpp
 * @brief Fixed provider dispatch for optional vendor enhancement backends.
 */
#include "hdr_backend_factory.h"
#include "hdr_enhanced/nvidia_rtx_video/truehdr_filter.h"
#include "hdr_enhanced/nvidia_dlssnr/dlssnr_filter.h"

namespace platf::dxgi {
  std::unique_ptr<pre_encode_filter_t>
  make_hdr_backend(
    std::string_view id, ID3D11Device *device, ID3D11DeviceContext *context,
    const std::filesystem::path &path, const pre_encode_filter_config_t &config,
    std::string_view runtime_digest, std::string &error) {
    if (id == "alkaidlab.nvidia_rtx_video") return hdr_enhanced::nvidia_rtx_video::truehdr::make_filter(device, context, path, config, error);
    if (id == "alkaidlab.nvidia_dlssnr") return hdr_enhanced::nvidia_dlssnr::make_filter(device, context, path, config, runtime_digest, error);
    error = "backend_not_registered";
    return {};
  }
}  // namespace platf::dxgi
