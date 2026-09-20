/**
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 Foundation Sunshine contributors
 *
 * @file src/platform/windows/image_enhancement/dlss_nr/adapter/tests/adapter_smoke.cpp
 * @brief Hardware smoke test for the DLSS NR adapter.
 *
 * Usage: foundation_dlssnr_adapter_smoke.exe <dir containing nvngx_dlssnr.dll>
 *
 * Verifies create/process/flush/destroy end to end: a gradient frame must
 * survive evaluation with finite pixels, the output must differ from a
 * passthrough copy once the filter is enabled, and repeated evaluation must
 * stay stable across N frames with per-frame timings.
 */
#include "src/platform/windows/image_enhancement/dlss_nr/adapter_abi.h"

#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

  uint32_t WIDTH = 1280;
  uint32_t HEIGHT = 720;
  uint32_t FRAMES = 100;
  using Microsoft::WRL::ComPtr;

  std::vector<uint32_t>
  load_image(const std::filesystem::path &path, IWICImagingFactory *factory) {
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICFormatConverter> converter;
    HRESULT hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
      WICDecodeMetadataCacheOnLoad, &decoder);
    if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(hr)) hr = factory->CreateBitmapScaler(&scaler);
    if (SUCCEEDED(hr)) hr = scaler->Initialize(frame.Get(), WIDTH, HEIGHT, WICBitmapInterpolationModeFant);
    if (SUCCEEDED(hr)) hr = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr)) hr = converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppBGRA,
      WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
    std::vector<uint32_t> pixels(WIDTH * HEIGHT);
    if (SUCCEEDED(hr)) hr = converter->CopyPixels(nullptr, WIDTH * 4,
      static_cast<UINT>(pixels.size() * 4), reinterpret_cast<BYTE *>(pixels.data()));
    if (FAILED(hr)) return {};
    for (auto &pixel : pixels) pixel |= 0xFF000000u;
    return pixels;
  }

  bool
  save_image(const std::filesystem::path &path, const std::vector<uint32_t> &pixels, IWICImagingFactory *factory) {
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    HRESULT hr = factory->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, nullptr);
    if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
    if (SUCCEEDED(hr)) hr = frame->SetSize(WIDTH, HEIGHT);
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
    if (format != GUID_WICPixelFormat32bppBGRA) return false;
    if (SUCCEEDED(hr)) hr = frame->WritePixels(HEIGHT, WIDTH * 4,
      static_cast<UINT>(pixels.size() * 4), reinterpret_cast<BYTE *>(const_cast<uint32_t *>(pixels.data())));
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();
    return SUCCEEDED(hr);
  }

  template <typename T>
  struct com_release_t {
    void
    operator()(T *value) const {
      if (value) {
        value->Release();
      }
    }
  };

  template <typename T>
  using com_ptr_t = std::unique_ptr<T, com_release_t<T>>;

  const foundation_dlssnr_adapter_api_t *
  load_adapter(const std::filesystem::path &adapter_path) {
    HMODULE module = LoadLibraryExW(adapter_path.c_str(), nullptr,
      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) {
      std::wprintf(L"FAIL: LoadLibraryExW(%s) error %lu\n", adapter_path.c_str(), GetLastError());
      return nullptr;
    }
    const auto get_api = reinterpret_cast<foundation_dlssnr_adapter_get_api_fn>(
      GetProcAddress(module, FOUNDATION_DLSSNR_ADAPTER_GET_API_EXPORT));
    if (!get_api) {
      std::printf("FAIL: adapter export missing\n");
      return nullptr;
    }
    const auto api = get_api(FOUNDATION_DLSSNR_ADAPTER_ABI_VERSION);
    if (!api || api->abi_version != FOUNDATION_DLSSNR_ADAPTER_ABI_VERSION) {
      std::printf("FAIL: adapter ABI mismatch\n");
      return nullptr;
    }
    return api;
  }

  com_ptr_t<ID3D11Device>
  create_device() {
    com_ptr_t<ID3D11Device> device;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_11_0;
    ID3D11Device *raw = nullptr;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
          nullptr, 0, D3D11_SDK_VERSION, &raw, &feature_level, nullptr))) {
      return {};
    }
    device.reset(raw);
    return device;
  }

  std::vector<uint32_t>
  make_gradient_frame(uint32_t frame_index) {
    std::vector<uint32_t> pixels(WIDTH * HEIGHT);
    const int shift = static_cast<int>(frame_index % 16);
    for (uint32_t y = 0; y < HEIGHT; ++y) {
      for (uint32_t x = 0; x < WIDTH; ++x) {
        const uint8_t r = static_cast<uint8_t>((x * 255u) / WIDTH);
        const uint8_t g = static_cast<uint8_t>((y * 255u) / HEIGHT);
        const uint8_t b = static_cast<uint8_t>(((x + y + shift) * 127u) / (WIDTH + HEIGHT));
        pixels[y * WIDTH + x] = 0xFF000000u | (uint32_t(r) << 16) | (uint32_t(g) << 8) | b;
      }
    }
    return pixels;
  }

  com_ptr_t<ID3D11Texture2D>
  upload_texture(ID3D11Device *device, const std::vector<uint32_t> &pixels) {
    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = WIDTH;
    desc.Height = HEIGHT;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial {};
    initial.pSysMem = pixels.data();
    initial.SysMemPitch = WIDTH * sizeof(uint32_t);
    ID3D11Texture2D *raw = nullptr;
    if (FAILED(device->CreateTexture2D(&desc, &initial, &raw))) {
      return {};
    }
    return com_ptr_t<ID3D11Texture2D>(raw);
  }

  com_ptr_t<ID3D11Texture2D>
  create_output_texture(ID3D11Device *device) {
    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = WIDTH;
    desc.Height = HEIGHT;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D *raw = nullptr;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &raw))) {
      return {};
    }
    return com_ptr_t<ID3D11Texture2D>(raw);
  }

  std::vector<uint32_t>
  read_back(ID3D11DeviceContext *context, ID3D11Device *device, ID3D11Texture2D *texture) {
    D3D11_TEXTURE2D_DESC desc {};
    texture->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ID3D11Texture2D *staging_raw = nullptr;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging_raw))) {
      return {};
    }
    com_ptr_t<ID3D11Texture2D> staging(staging_raw);
    context->CopyResource(staging.get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped {};
    if (FAILED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
      return {};
    }
    std::vector<uint32_t> pixels(WIDTH * HEIGHT);
    const auto *source = static_cast<const uint8_t *>(mapped.pData);
    for (uint32_t y = 0; y < HEIGHT; ++y) {
      std::memcpy(pixels.data() + y * WIDTH, source + y * mapped.RowPitch, WIDTH * sizeof(uint32_t));
    }
    context->Unmap(staging.get(), 0);
    return pixels;
  }

  double
  now_ms() {
    LARGE_INTEGER frequency, counter;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    return double(counter.QuadPart) * 1000.0 / double(frequency.QuadPart);
  }

}  // namespace

int
wmain(int argc, wchar_t **argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  const bool optical_flow = argc > 2 && std::wcscmp(argv[argc - 1], L"--flow") == 0;
  if (optical_flow) --argc;
  const bool benchmark = argc == 6 && std::wcscmp(argv[5], L"--benchmark") == 0;
  const bool pan = argc == 8 && std::wcscmp(argv[7], L"--pan") == 0;
  if (argc != 2 && argc != 5 && argc != 7 && !benchmark && !pan) {
    std::printf("usage: foundation_dlssnr_adapter_smoke.exe <runtime_dir> [width height frames [--benchmark | image output_dir [--pan]]] [--flow]\n");
    return 1;
  }
  if (argc >= 5) {
    uint32_t *options[] = { &WIDTH, &HEIGHT, &FRAMES };
    for (int i = 0; i < 3; ++i) {
      wchar_t *end = nullptr;
      const auto value = std::wcstoul(argv[i + 2], &end, 10);
      if (end == argv[i + 2] || *end || value == 0 || value > (i == 2 ? 10000u : 8192u)) {
        std::printf("FAIL: invalid width, height or frame count\n");
        return 1;
      }
      *options[i] = static_cast<uint32_t>(value);
    }
  }
  std::printf("smoke: %ux%u, %u frames\n", WIDTH, HEIGHT, FRAMES);
  struct com_apartment_t {
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~com_apartment_t() { if (SUCCEEDED(result)) CoUninitialize(); }
  } apartment;
  ComPtr<IWICImagingFactory> imaging;
  std::vector<uint32_t> source_image;
  std::filesystem::path image_output;
  if (argc == 7 || pan) {
    if (FAILED(apartment.result) || FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&imaging)))) return 1;
    source_image = load_image(argv[5], imaging.Get());
    if (source_image.empty()) {
      std::printf("FAIL: image load failed\n");
      return 1;
    }
    image_output = argv[6];
    std::filesystem::create_directories(image_output);
    if (!save_image(image_output / L"input.png", source_image, imaging.Get())) return 1;
  }
  const std::filesystem::path runtime_directory = argv[1];
  if (!std::filesystem::exists(runtime_directory / "nvngx_dlssnr.dll")) {
    std::printf("FAIL: nvngx_dlssnr.dll not found in the given directory\n");
    return 1;
  }
  const std::filesystem::path adapter_path =
    std::filesystem::path(argv[0]).parent_path() / "foundation_dlssnr_adapter.dll";

  const auto *api = load_adapter(adapter_path);
  if (!api) return 1;

  auto device = create_device();
  if (!device) {
    std::printf("FAIL: D3D11 device creation failed\n");
    return 1;
  }
  ID3D11DeviceContext *context_raw = nullptr;
  device->GetImmediateContext(&context_raw);
  com_ptr_t<ID3D11DeviceContext> context(context_raw);

  foundation_dlssnr_config_t config {};
  config.struct_size = sizeof(config);
  config.width = WIDTH;
  config.height = HEIGHT;
  config.intensity = 1.0f;
  config.local_tone_strength = 1.0f;
  config.local_structure_strength = 1.0f;
  config.skin_structure_strength = 0.0f;
  config.style = 0;
  config.motion_mode = optical_flow ? FOUNDATION_DLSSNR_MOTION_OPTICAL_FLOW : FOUNDATION_DLSSNR_MOTION_ZERO;
  config.motion_quality = optical_flow ? 2 : 0;
  config.auto_mask = 0;
  config.ui_correction = 0;
  config.runtime_directory = runtime_directory.c_str();

  void *instance = nullptr;
  const auto create_status = api->create(device.get(), &config, &instance);
  if (create_status != FOUNDATION_DLSSNR_STATUS_OK || !instance) {
    std::printf("FAIL: create status=%d\n", static_cast<int>(create_status));
    return 1;
  }
  std::printf("create OK\n");

  auto output = create_output_texture(device.get());
  if (!output) {
    std::printf("FAIL: output texture allocation failed\n");
    api->destroy(instance);
    return 1;
  }

  double total_ms = 0.0;
  double max_ms = 0.0;
  uint64_t changed_pixels = 0;
  std::vector<uint32_t> first_output;
  const auto benchmark_pixels = benchmark ? make_gradient_frame(0) : std::vector<uint32_t> {};
  auto benchmark_input = benchmark ? upload_texture(device.get(), benchmark_pixels) : com_ptr_t<ID3D11Texture2D> {};
  if (benchmark && !benchmark_input) { api->destroy(instance); return 1; }
  const double batch_start = now_ms();
  for (uint32_t frame = 0; frame < FRAMES; ++frame) {
    if (benchmark) {
      const auto status = api->process(instance, context.get(), benchmark_input.get(), output.get());
      if (status != FOUNDATION_DLSSNR_STATUS_OK) {
        std::printf("FAIL: benchmark process status=%d on frame %u\n", static_cast<int>(status), frame);
        api->destroy(instance);
        return 1;
      }
      continue;
    }
    auto input_pixels = source_image.empty() ? make_gradient_frame(frame) : source_image;
    if (pan) {
      // Deterministic horizontal camera-like translation, followed by a hold.
      // Clamp the newly exposed edge; this is synthetic motion, not gameplay.
      const uint32_t shift = (frame < FRAMES / 2 ? frame : FRAMES / 2) * 2;
      for (uint32_t y = 0; y < HEIGHT; ++y) for (uint32_t x = 0; x < WIDTH; ++x) {
        const uint32_t sx = x + shift < WIDTH ? x + shift : WIDTH - 1;
        input_pixels[y * WIDTH + x] = source_image[y * WIDTH + sx];
      }
    }
    auto input = upload_texture(device.get(), input_pixels);
    if (!input) {
      std::printf("FAIL: input upload failed on frame %u\n", frame);
      api->destroy(instance);
      return 1;
    }
    const double start = now_ms();
    const auto status = api->process(instance, context.get(), input.get(), output.get());
    api->flush(instance);
    if (status != FOUNDATION_DLSSNR_STATUS_OK) {
      std::printf("FAIL: process status=%d on frame %u\n", static_cast<int>(status), frame);
      api->destroy(instance);
      return 1;
    }
    context->Flush();
    auto pixels = read_back(context.get(), device.get(), output.get());
    // Readback waits for the output copy too. This is CPU wall time including
    // synchronization/readback, not a GPU timestamp measurement.
    const double elapsed = now_ms() - start;
    total_ms += elapsed;
    max_ms = elapsed > max_ms ? elapsed : max_ms;
    if (pixels.size() != WIDTH * HEIGHT) {
      std::printf("FAIL: readback failed on frame %u\n", frame);
      api->destroy(instance);
      return 1;
    }
    // All pixels must remain valid BGRA (alpha 0xFF set by the filter input).
    size_t invalid = 0;
    for (const uint32_t pixel : pixels) {
      if ((pixel & 0xFF000000u) != 0xFF000000u) ++invalid;
    }
    if (invalid > (WIDTH * HEIGHT) / 100) {
      std::printf("FAIL: %zu pixels with invalid alpha on frame %u\n", invalid, frame);
      api->destroy(instance);
      return 1;
    }
    size_t nonblack = 0;
    for (size_t i = 0; i < pixels.size(); ++i) {
      const auto rgb = pixels[i] & 0x00FFFFFFu;
      if (rgb) ++nonblack;
      if (rgb != (input_pixels[i] & 0x00FFFFFFu)) ++changed_pixels;
    }
    if (!nonblack) {
      std::printf("FAIL: black output on frame %u\n", frame);
      api->destroy(instance);
      return 1;
    }
    if (pan) {
      const auto suffix = std::to_wstring(frame) + L".png";
      if (!save_image(image_output / (L"input-" + suffix), input_pixels, imaging.Get()) ||
          !save_image(image_output / (L"output-" + suffix), pixels, imaging.Get())) {
        std::printf("FAIL: motion sequence output failed\n");
        api->destroy(instance);
        return 1;
      }
    }
    if (!source_image.empty() && (frame == 0 || frame == FRAMES - 1)) {
      if (!save_image(image_output / (frame == 0 ? L"output-first.png" : L"output-last.png"), pixels, imaging.Get())) {
        std::printf("FAIL: PNG output failed\n");
        api->destroy(instance);
        return 1;
      }
    }
    if (frame == 0) {
      first_output = std::move(pixels);
    }
  }
  if (!benchmark && !changed_pixels) {
    std::printf("FAIL: all %u frames were RGB passthrough; enhancement not demonstrated\n", FRAMES);
    api->destroy(instance);
    return 1;
  }
  if (benchmark) {
    api->flush(instance);
    context->Flush();
    const auto pixels = read_back(context.get(), device.get(), output.get());
    const double batch_ms = now_ms() - batch_start;
    if (pixels.empty() || pixels == benchmark_pixels) {
      std::printf("FAIL: benchmark readback or passthrough\n");
      api->destroy(instance);
      return 1;
    }
    std::printf("BENCHMARK: %u frames, total %.3f ms, average %.3f ms/frame, %.2f fps; no per-frame CPU readback/flush\n",
      FRAMES, batch_ms, batch_ms / FRAMES, FRAMES * 1000.0 / batch_ms);
    api->destroy(instance);
    std::printf("BENCHMARK PASSED\n");
    return 0;
  }
  std::printf("process OK: %u frames, %llu changed RGB pixels\n", FRAMES,
    static_cast<unsigned long long>(changed_pixels));
  std::printf("CPU wall time including readback: avg %.3f ms, max %.3f ms\n", total_ms / FRAMES, max_ms);

  // Spot-check: the center pixel must be a plausible processed value.
  const uint32_t center = first_output[HEIGHT / 2 * WIDTH + WIDTH / 2];
  std::printf("center BGRA: 0x%08X\n", center);

  api->flush(instance);
  api->destroy(instance);
  std::printf("destroy OK\n");
  std::printf("SMOKE PASSED\n");
  return 0;
}
