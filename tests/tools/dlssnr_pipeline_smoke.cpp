/** SPDX-License-Identifier: GPL-3.0-only
 * Opt-in hardware check of the production factory, verified loader and NR filter.
 * Usage: dlssnr_pipeline_smoke <absolute adapter path> <runtime SHA-256>
 */
#include "src/platform/windows/pre_encode_filter.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dxgi.h>
#include <memory>
#include <vector>

template <class T>
struct release_com {
  void
  operator()(T *value) const {
    if (value) value->Release();
  }
};
template <class T>
using com_ptr = std::unique_ptr<T, release_com<T>>;

int
main(int argc, char **argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc < 3) return 2;
  bool hdr = false, zero = false, flow = false, uhd = false, shared = false;
  for (int i = 3; i < argc; ++i) {
    if (std::strcmp(argv[i], "--hdr") == 0)
      hdr = true;
    else if (std::strcmp(argv[i], "--zero") == 0)
      zero = true;
    else if (std::strcmp(argv[i], "--flow") == 0)
      flow = true;
    else if (std::strcmp(argv[i], "--4k") == 0)
      uhd = true;
    else if (std::strcmp(argv[i], "--shared") == 0)
      shared = true;
    else
      return 2;
  }
  platf::pre_encode_filter_config_t config;
  config.nr_intensity = zero ? 0.0f : 1.0f;
  config.nr_motion_quality = flow ? 2 : 0;
  ID3D11Device *raw_device = nullptr;
  ID3D11DeviceContext *raw_context = nullptr;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, shared ? D3D11_CREATE_DEVICE_VIDEO_SUPPORT : 0,
        nullptr, 0, D3D11_SDK_VERSION, &raw_device, nullptr, &raw_context))) return 1;
  com_ptr<ID3D11Device> device(raw_device);
  com_ptr<ID3D11DeviceContext> context(raw_context);
  com_ptr<ID3D11Device> producer_device;
  com_ptr<ID3D11DeviceContext> producer_context;
  if (shared) {
    raw_device = nullptr;
    raw_context = nullptr;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
          nullptr, 0, D3D11_SDK_VERSION, &raw_device, nullptr, &raw_context))) return 1;
    producer_device.reset(raw_device);
    producer_context.reset(raw_context);
  }
  for (int session = 0; session < 3; ++session) {
    auto filter = platf::dxgi::make_pre_encode_filter(
      platf::pre_encode_filter_e::external_neural_enhancement, device.get(), context.get(),
      std::filesystem::path(reinterpret_cast<const char8_t *>(argv[1])), config, "alkaidlab.nvidia_dlssnr", argv[2]);
    if (!filter || filter->degraded()) {
      std::fprintf(stderr, "Factory failed: %s\n", filter ? std::string(filter->failure_reason()).c_str() : "null");
      return 1;
    }
    // Resize the same filter, then recreate the entire filter next session.
    for (const UINT width : { uhd ? 1920u : 1280u, uhd ? 3840u : 1920u }) {
      const UINT height = width * 9 / 16;
      std::vector<uint32_t> pixels(width * height);
      std::vector<uint16_t> hdr_pixels(width * height * 4);
      const uint16_t pattern[] { 0xb000, 0x3400, 0x4a40, 0x3800, 0x5240, 0x3c00, 0, 0x3c00 };
      for (size_t i = 0; i < hdr_pixels.size(); ++i) hdr_pixels[i] = pattern[(i / 256 * 4 + i % 4) % 8];
      for (UINT y = 0; y < height; ++y)
        for (UINT x = 0; x < width; ++x)
          pixels[y * width + x] = 0xff000000u | ((x * 255 / width) << 16) | ((y * 255 / height) << 8) | 64u;
      D3D11_TEXTURE2D_DESC desc {};
      desc.Width = width;
      desc.Height = height;
      desc.MipLevels = 1;
      desc.ArraySize = 1;
      desc.Format = hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;
      desc.SampleDesc.Count = 1;
      desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      D3D11_SUBRESOURCE_DATA data { pixels.data(), width * 4, 0 };
      if (hdr) {
        data.pSysMem = hdr_pixels.data();
        data.SysMemPitch = width * 8;
      }
      ID3D11Texture2D *raw = nullptr;
      if (FAILED(device->CreateTexture2D(&desc, &data, &raw))) return 1;
      com_ptr<ID3D11Texture2D> input(raw);
      ID3D11ShaderResourceView *raw_srv = nullptr;
      if (FAILED(device->CreateShaderResourceView(input.get(), nullptr, &raw_srv))) return 1;
      com_ptr<ID3D11ShaderResourceView> srv(raw_srv);
      com_ptr<ID3D11Texture2D> capture;
      com_ptr<IDXGIKeyedMutex> capture_mutex;
      com_ptr<ID3D11Texture2D> producer_texture;
      com_ptr<IDXGIKeyedMutex> producer_mutex;
      if (shared) {
        auto shared_desc = desc;
        shared_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        raw = nullptr;
        if (FAILED(producer_device->CreateTexture2D(&shared_desc, &data, &raw))) return 1;
        producer_texture.reset(raw);
        IDXGIResource *raw_resource = nullptr;
        if (FAILED(producer_texture->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void **>(&raw_resource)))) return 1;
        com_ptr<IDXGIResource> resource(raw_resource);
        HANDLE handle = nullptr;
        if (FAILED(resource->GetSharedHandle(&handle))) return 1;
        raw = nullptr;
        if (FAILED(device->OpenSharedResource(handle, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&raw)))) return 1;
        capture.reset(raw);
        IDXGIKeyedMutex *raw_mutex = nullptr;
        if (FAILED(capture->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void **>(&raw_mutex)))) return 1;
        capture_mutex.reset(raw_mutex);
        raw_mutex = nullptr;
        if (FAILED(producer_texture->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void **>(&raw_mutex)))) return 1;
        producer_mutex.reset(raw_mutex);
      }
      platf::dxgi::gpu_frame_view_t view {
        .texture = input.get(),
        .srv = srv.get(),
        .format = desc.Format,
        .semantic = { .domain = platf::frame_domain_e::sdr_rec709,
          .encoding = platf::pixel_encoding_class_e::unorm8,
          .reference_white_nits = 80.0f },
        .width = width,
        .height = height,
      };
      if (hdr) {
        view.semantic.domain = platf::frame_domain_e::linear_scrgb;
        view.semantic.encoding = platf::pixel_encoding_class_e::float16;
      }
      platf::dxgi::filter_result_t result;
      const auto start = std::chrono::steady_clock::now();
      for (int frame = 0; frame < 100; ++frame) {
        if (shared) {
          // Reproduce capture ownership -> private handoff -> model submission.
          // The model must never retain the shared capture mutex while waiting
          // on its D3D12 queue. Readback below also verifies first-frame progress.
          if (producer_mutex->AcquireSync(0, 5000) != S_OK) return 1;
          producer_context->UpdateSubresource(producer_texture.get(), 0, nullptr, data.pSysMem, data.SysMemPitch, 0);
          if (FAILED(producer_mutex->ReleaseSync(1))) return 1;
          if (capture_mutex->AcquireSync(1, 5000) != S_OK) return 1;
          context->CopyResource(input.get(), capture.get());
          if (FAILED(capture_mutex->ReleaseSync(0))) return 1;
        }
        result = filter->process(view);
        if (filter->degraded() || result.status != platf::dxgi::filter_status_e::ready ||
            result.frame.texture == input.get() || result.frame.semantic.domain != view.semantic.domain || result.frame.format != desc.Format) {
          std::fprintf(stderr, "Process failed: %s\n", std::string(filter->failure_reason()).c_str());
          return 1;
        }
        if (frame == 0) {
          auto readback_desc = desc;
          readback_desc.Usage = D3D11_USAGE_STAGING;
          readback_desc.BindFlags = 0;
          readback_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
          raw = nullptr;
          if (FAILED(device->CreateTexture2D(&readback_desc, nullptr, &raw))) return 1;
          com_ptr<ID3D11Texture2D> first_readback(raw);
          context->CopyResource(first_readback.get(), result.frame.texture);
          D3D11_MAPPED_SUBRESOURCE first_map {};
          if (FAILED(context->Map(first_readback.get(), 0, D3D11_MAP_READ, 0, &first_map))) return 1;
          context->Unmap(first_readback.get(), 0);
          std::printf("first_frame=%ux%u shared=%d elapsed=%.3f ms\n", width, height, shared,
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        }
      }
      filter->flush();
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      desc.Usage = D3D11_USAGE_STAGING;
      desc.BindFlags = 0;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      raw = nullptr;
      if (FAILED(device->CreateTexture2D(&desc, nullptr, &raw))) return 1;
      com_ptr<ID3D11Texture2D> staging(raw);
      context->CopyResource(staging.get(), result.frame.texture);
      D3D11_MAPPED_SUBRESOURCE mapped {};
      if (FAILED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) return 1;
      size_t changed = 0, nonblack = 0, highlights = 0;
      for (UINT y = 0; y < height; ++y) {
        if (hdr) {
          const auto row = reinterpret_cast<const uint16_t *>(static_cast<const char *>(mapped.pData) + y * mapped.RowPitch);
          for (UINT x = 0; x < width * 4; ++x) {
            changed += row[x] != hdr_pixels[y * width * 4 + x];
            nonblack += (row[x] & 0x7fffu) != 0;
            if (x % 4 != 3) highlights += row[x] > 0x3c00 && row[x] < 0x7c00;
          }
          continue;
        }
        const auto row = reinterpret_cast<const uint32_t *>(static_cast<const char *>(mapped.pData) + y * mapped.RowPitch);
        for (UINT x = 0; x < width; ++x) {
          changed += (row[x] & 0xffffffu) != (pixels[y * width + x] & 0xffffffu);
          nonblack += (row[x] & 0xffffffu) != 0;
        }
      }
      context->Unmap(staging.get(), 0);
      if (!nonblack || (zero ? changed != 0 : changed == 0) || (hdr && !highlights)) return 1;
      std::printf("session=%d size=%ux%u frames=100 average=%.3f ms (includes initialization) changed=%zu\n", session, width, height, ms / 100, changed);
      // Queue another frame and let resize/destruction drain it without a caller flush.
      filter->process(view);
    }
  }
  std::puts("PIPELINE SMOKE PASSED: verified loader, real NR, resize and repeated sessions");
  return 0;
}
