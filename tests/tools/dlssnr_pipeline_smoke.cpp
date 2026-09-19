/** SPDX-License-Identifier: GPL-3.0-only
 * Opt-in hardware check of the production factory, verified loader and NR filter.
 * Usage: dlssnr_pipeline_smoke <absolute adapter path> <runtime SHA-256>
 */
#include <chrono>
#include <cstdio>
#include <memory>
#include <vector>
#include "src/platform/windows/pre_encode_filter.h"

template<class T> struct release_com {
  void operator()(T *value) const { if (value) value->Release(); }
};
template<class T> using com_ptr = std::unique_ptr<T, release_com<T>>;

int main(int argc, char **argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc != 3) return 2;
  ID3D11Device *raw_device = nullptr;
  ID3D11DeviceContext *raw_context = nullptr;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
      nullptr, 0, D3D11_SDK_VERSION, &raw_device, nullptr, &raw_context))) return 1;
  com_ptr<ID3D11Device> device(raw_device);
  com_ptr<ID3D11DeviceContext> context(raw_context);
  for (int session = 0; session < 3; ++session) {
    auto filter = platf::dxgi::make_pre_encode_filter(
      platf::pre_encode_filter_e::external_sdr_to_sdr_nr, device.get(), context.get(),
      std::filesystem::path(reinterpret_cast<const char8_t *>(argv[1])), {}, "alkaidlab.nvidia_dlssnr", argv[2]);
    if (!filter || filter->degraded()) {
      std::fprintf(stderr, "Factory failed: %s\n", filter ? std::string(filter->failure_reason()).c_str() : "null");
      return 1;
    }
    // Resize the same filter, then recreate the entire filter next session.
    for (const UINT width : { 1280u, 1920u }) {
      const UINT height = width * 9 / 16;
      std::vector<uint32_t> pixels(width * height);
      for (UINT y = 0; y < height; ++y) for (UINT x = 0; x < width; ++x)
        pixels[y * width + x] = 0xff000000u | ((x * 255 / width) << 16) | ((y * 255 / height) << 8) | 64u;
      D3D11_TEXTURE2D_DESC desc {};
      desc.Width = width; desc.Height = height; desc.MipLevels = 1; desc.ArraySize = 1;
      desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
      desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      D3D11_SUBRESOURCE_DATA data { pixels.data(), width * 4, 0 };
      ID3D11Texture2D *raw = nullptr;
      if (FAILED(device->CreateTexture2D(&desc, &data, &raw))) return 1;
      com_ptr<ID3D11Texture2D> input(raw);
      ID3D11ShaderResourceView *raw_srv = nullptr;
      if (FAILED(device->CreateShaderResourceView(input.get(), nullptr, &raw_srv))) return 1;
      com_ptr<ID3D11ShaderResourceView> srv(raw_srv);
      platf::dxgi::gpu_frame_view_t view {
        .texture = input.get(), .srv = srv.get(), .format = desc.Format,
        .semantic = { .domain = platf::frame_domain_e::sdr_rec709,
          .encoding = platf::pixel_encoding_class_e::unorm8, .reference_white_nits = 80.0f },
        .width = width, .height = height,
      };
      platf::dxgi::filter_result_t result;
      const auto start = std::chrono::steady_clock::now();
      for (int frame = 0; frame < 100; ++frame) {
        result = filter->process(view);
        if (filter->degraded() || result.status != platf::dxgi::filter_status_e::ready ||
            result.frame.texture == input.get() || result.frame.semantic.domain != platf::frame_domain_e::sdr_rec709) {
          std::fprintf(stderr, "Process failed: %s\n", std::string(filter->failure_reason()).c_str());
          return 1;
        }
      }
      filter->flush();
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      raw = nullptr;
      if (FAILED(device->CreateTexture2D(&desc, nullptr, &raw))) return 1;
      com_ptr<ID3D11Texture2D> staging(raw);
      context->CopyResource(staging.get(), result.frame.texture);
      D3D11_MAPPED_SUBRESOURCE mapped {};
      if (FAILED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) return 1;
      size_t changed = 0, nonblack = 0;
      for (UINT y = 0; y < height; ++y) {
        const auto row = reinterpret_cast<const uint32_t *>(static_cast<const char *>(mapped.pData) + y * mapped.RowPitch);
        for (UINT x = 0; x < width; ++x) {
          changed += (row[x] & 0xffffffu) != (pixels[y * width + x] & 0xffffffu);
          nonblack += (row[x] & 0xffffffu) != 0;
        }
      }
      context->Unmap(staging.get(), 0);
      if (!changed || !nonblack) return 1;
      std::printf("session=%d size=%ux%u frames=100 average=%.3f ms (includes initialization) changed=%zu\n", session, width, height, ms / 100, changed);
      // Queue another frame and let resize/destruction drain it without a caller flush.
      filter->process(view);
    }
  }
  std::puts("PIPELINE SMOKE PASSED: verified loader, real NR, resize and repeated sessions");
  return 0;
}
