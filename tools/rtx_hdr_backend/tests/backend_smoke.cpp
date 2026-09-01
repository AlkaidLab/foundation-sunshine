#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <d3d11.h>
#include <dxgi.h>
#include <windows.h>

#include "src/platform/windows/rtx_hdr/backend_abi.h"

namespace {
  template <class T>
  void release(T *&value) {
    if (value) {
      value->Release();
      value = nullptr;
    }
  }

  int fail(const std::string &message) {
    std::cerr << "FAIL: " << message << '\n';
    return 1;
  }

  std::string adapter_name(ID3D11Device *device) {
    IDXGIDevice *dxgi_device = nullptr;
    IDXGIAdapter *adapter = nullptr;
    DXGI_ADAPTER_DESC desc {};
    std::string result = "unknown";
    if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dxgi_device))) &&
        SUCCEEDED(dxgi_device->GetAdapter(&adapter)) &&
        SUCCEEDED(adapter->GetDesc(&desc))) {
      char buffer[256] {};
      WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, buffer, sizeof(buffer), nullptr, nullptr);
      result = buffer;
    }
    release(adapter);
    release(dxgi_device);
    return result;
  }
}

int wmain(int argc, wchar_t **argv) {
  const std::filesystem::path backend_path = argc > 1
    ? std::filesystem::absolute(argv[1])
    : std::filesystem::absolute(L"foundation_truehdr_backend.dll");
  if (!std::filesystem::is_regular_file(backend_path)) {
    return fail("backend DLL does not exist: " + backend_path.string());
  }

  const auto module = LoadLibraryExW(
    backend_path.c_str(),
    nullptr,
    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  if (!module) {
    return fail("LoadLibraryExW failed with error " + std::to_string(GetLastError()));
  }
  const auto get_api = reinterpret_cast<foundation_truehdr_get_api_fn>(
    GetProcAddress(module, FOUNDATION_TRUEHDR_GET_API_EXPORT));
  const auto *api = get_api ? get_api(FOUNDATION_TRUEHDR_ABI_VERSION) : nullptr;
  if (!api || api->abi_version != FOUNDATION_TRUEHDR_ABI_VERSION ||
      api->struct_size < sizeof(foundation_truehdr_api_t)) {
    FreeLibrary(module);
    return fail("backend ABI negotiation failed");
  }

  ID3D11Device *device = nullptr;
  ID3D11DeviceContext *context = nullptr;
  D3D_FEATURE_LEVEL selected_level {};
  const D3D_FEATURE_LEVEL requested_levels[] {
    D3D_FEATURE_LEVEL_11_1,
    D3D_FEATURE_LEVEL_11_0,
  };
  const auto device_hr = D3D11CreateDevice(
    nullptr,
    D3D_DRIVER_TYPE_HARDWARE,
    nullptr,
    D3D11_CREATE_DEVICE_BGRA_SUPPORT,
    requested_levels,
    static_cast<UINT>(std::size(requested_levels)),
    D3D11_SDK_VERSION,
    &device,
    &selected_level,
    &context);
  if (FAILED(device_hr)) {
    FreeLibrary(module);
    return fail("D3D11CreateDevice failed with HRESULT " + std::to_string(device_hr));
  }

  constexpr UINT width = 1920;
  constexpr UINT height = 1080;
  std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width) * height);
  for (UINT y = 0; y < height; ++y) {
    for (UINT x = 0; x < width; ++x) {
      const auto red = static_cast<std::uint8_t>((255u * x) / (width - 1));
      const auto green = static_cast<std::uint8_t>((255u * y) / (height - 1));
      const auto blue = static_cast<std::uint8_t>((red + green) / 2u);
      pixels[static_cast<std::size_t>(y) * width + x] =
        0xff000000u | (static_cast<std::uint32_t>(blue) << 16u) |
        (static_cast<std::uint32_t>(green) << 8u) | red;
    }
  }

  D3D11_TEXTURE2D_DESC input_desc {};
  input_desc.Width = width;
  input_desc.Height = height;
  input_desc.MipLevels = 1;
  input_desc.ArraySize = 1;
  input_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  input_desc.SampleDesc.Count = 1;
  input_desc.Usage = D3D11_USAGE_DEFAULT;
  input_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA initial_data {};
  initial_data.pSysMem = pixels.data();
  initial_data.SysMemPitch = width * sizeof(std::uint32_t);
  ID3D11Texture2D *input = nullptr;
  auto hr = device->CreateTexture2D(&input_desc, &initial_data, &input);
  if (FAILED(hr)) {
    release(context);
    release(device);
    FreeLibrary(module);
    return fail("creating the SDR input texture failed");
  }

  D3D11_TEXTURE2D_DESC output_desc = input_desc;
  output_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  output_desc.BindFlags = D3D11_BIND_RENDER_TARGET |
                          D3D11_BIND_SHADER_RESOURCE |
                          D3D11_BIND_UNORDERED_ACCESS;
  ID3D11Texture2D *output = nullptr;
  hr = device->CreateTexture2D(&output_desc, nullptr, &output);
  ID3D11UnorderedAccessView *output_uav = nullptr;
  if (SUCCEEDED(hr)) {
    hr = device->CreateUnorderedAccessView(output, nullptr, &output_uav);
  }

  D3D11_TEXTURE2D_DESC staging_desc = output_desc;
  staging_desc.Usage = D3D11_USAGE_STAGING;
  staging_desc.BindFlags = 0;
  staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Texture2D *staging = nullptr;
  if (SUCCEEDED(hr)) {
    hr = device->CreateTexture2D(&staging_desc, nullptr, &staging);
  }
  if (FAILED(hr)) {
    release(output_uav);
    release(output);
    release(input);
    release(context);
    release(device);
    FreeLibrary(module);
    return fail("creating the scRGB output textures failed");
  }
  constexpr float clear_color[4] { 0.0f, 0.0f, 0.0f, 0.0f };
  context->ClearUnorderedAccessViewFloat(output_uav, clear_color);
  release(output_uav);

  foundation_truehdr_config_t config {};
  config.struct_size = sizeof(config);
  config.width = width;
  config.height = height;
  config.contrast = 0.0f;
  config.saturation = 0.0f;
  config.middle_gray_nits = 50.0f;
  config.peak_nits = 1000.0f;
  void *instance = nullptr;
  auto status = api->create(device, &config, &instance);
  if (status == FOUNDATION_TRUEHDR_STATUS_OK) {
    status = api->process(instance, context, input, output);
  }
  if (status == FOUNDATION_TRUEHDR_STATUS_OK) {
    api->flush(instance);
    context->CopyResource(staging, output);
  }

  bool meaningful_output = false;
  std::uint16_t center_rgb[3] {};
  if (status == FOUNDATION_TRUEHDR_STATUS_OK) {
    D3D11_MAPPED_SUBRESOURCE mapped {};
    hr = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (SUCCEEDED(hr)) {
      const auto *center_row = reinterpret_cast<const std::uint16_t *>(
        static_cast<const std::uint8_t *>(mapped.pData) + (height / 2u) * mapped.RowPitch);
      const auto center = static_cast<std::size_t>(width / 2u) * 4u;
      center_rgb[0] = center_row[center];
      center_rgb[1] = center_row[center + 1];
      center_rgb[2] = center_row[center + 2];
      for (UINT y = 0; y < height && !meaningful_output; ++y) {
        const auto *row = reinterpret_cast<const std::uint16_t *>(
          static_cast<const std::uint8_t *>(mapped.pData) + y * mapped.RowPitch);
        for (UINT x = 0; x < width && !meaningful_output; ++x) {
          for (UINT channel = 0; channel < 3; ++channel) {
            const auto value = row[static_cast<std::size_t>(x) * 4u + channel];
            const bool finite_positive = (value & 0x8000u) == 0 && (value & 0x7c00u) != 0x7c00u;
            if (finite_positive && value > 0x3c00u) {  // FP16 1.0
              meaningful_output = true;
              break;
            }
          }
        }
      }
      context->Unmap(staging, 0);
    }
  }

  if (instance) {
    api->destroy(instance);
  }
  release(staging);
  release(output);
  release(input);
  release(context);
  const auto gpu = adapter_name(device);
  release(device);
  FreeLibrary(module);

  if (status != FOUNDATION_TRUEHDR_STATUS_OK) {
    return fail("TrueHDR backend returned status " + std::to_string(status) + " on " + gpu);
  }
  if (FAILED(hr) || !meaningful_output) {
    return fail("TrueHDR completed but produced no readable HDR pixels on " + gpu);
  }
  std::cout << "PASS: NGX TrueHDR processed 1920x1080 SDR to FP16 scRGB on " << gpu
            << "; center RGB half bits=" << center_rgb[0] << ',' << center_rgb[1] << ','
            << center_rgb[2] << '\n';
  return 0;
}
