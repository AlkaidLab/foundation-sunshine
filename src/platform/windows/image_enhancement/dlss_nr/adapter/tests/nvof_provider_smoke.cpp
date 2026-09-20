#include "../src/nvof_provider.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
float
half(short bits) {
  unsigned value = static_cast<unsigned short>(bits), exponent = (value >> 10) & 31, mantissa = value & 1023;
  float result = exponent ? std::ldexp(float(1024 + mantissa), int(exponent) - 25) : std::ldexp(float(mantissa), -24);
  return value & 32768 ? -result : result;
}
int
main(int argc, char **argv) {
  const UINT w = argc > 1 ? std::atoi(argv[1]) : 1920, h = argc > 2 ? std::atoi(argv[2]) : 1080;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context))) return 1;
  D3D11_TEXTURE2D_DESC desc {};
  desc.Width = w;
  desc.Height = h;
  desc.MipLevels = desc.ArraySize = 1;
  desc.SampleDesc.Count = 1;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  ComPtr<ID3D11Texture2D> source, output, staging;
  if (FAILED(device->CreateTexture2D(&desc, nullptr, &source))) return 2;
  desc.Format = DXGI_FORMAT_R16G16_FLOAT;
  desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
  if (FAILED(device->CreateTexture2D(&desc, nullptr, &output))) return 3;
  desc.BindFlags = 0;
  desc.Usage = D3D11_USAGE_STAGING;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) return 4;
  D3D11_VIEWPORT sentinel { 7, 9, 211, 123, 0.2f, 0.8f };
  context->RSSetViewports(1, &sentinel);
  auto checkState = [&]() {UINT count=1;D3D11_VIEWPORT now{};context->RSGetViewports(&count,&now);return count==1&&now.TopLeftX==7&&now.TopLeftY==9&&now.Width==211&&now.Height==123&&now.MinDepth==0.2f&&now.MaxDepth==0.8f; };
  auto flow = flow_provider::create(device.Get(), context.Get(), source.Get(), output.Get(), 2);
  if (!flow) {
    std::puts("create failed");
    return 5;
  }
  if (!checkState()) {
    std::puts("create changed caller viewport");
    return 6;
  }
  std::vector<unsigned> pixels(w * h);
  std::vector<double> times;
  for (int frame = 0; frame < 24; ++frame) {
    int dx = 8 * (frame % 6), dy = -8 * (frame % 6);
    bool reset = frame % 6 == 0;
    for (UINT y = 0; y < h; ++y)
      for (UINT x = 0; x < w; ++x) {
        auto sx = UINT((int(x) + int(w) - dx) % int(w)), sy = UINT((int(y) + int(h) - dy) % int(h));
        unsigned v = (sx / 8) * 374761393u + (sy / 8) * 668265263u;
        v = (v ^ (v >> 13)) * 1274126177u;
        pixels[y * w + x] = 0xff000000u | (v & 0xffffffu);
      }
    context->UpdateSubresource(source.Get(), 0, nullptr, pixels.data(), w * 4, 0);
    const auto start = std::chrono::steady_clock::now();
    if (!flow->process(reset)) return 7;
    if (!checkState()) {
      std::puts("process changed caller viewport");
      return 8;
    }
    context->CopyResource(staging.Get(), output.Get());
    D3D11_MAPPED_SUBRESOURCE mapped {};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 9;
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (frame > 5 && !reset) times.push_back(ms);
    std::vector<float> xs, ys;
    float maximum = 0;
    size_t inaccurate = 0, innerBad = 0;
    UINT maxX = 0, maxY = 0;
    for (UINT y = 32; y < h - 32; ++y) {
      const auto *row = reinterpret_cast<const short *>(static_cast<const char *>(mapped.pData) + y * mapped.RowPitch);
      for (UINT x = 32; x < w - 32; ++x) {
        float vx = half(row[2 * x]), vy = half(row[2 * x + 1]);
        xs.push_back(vx);
        ys.push_back(vy);
        float magnitude = std::max(std::abs(vx), std::abs(vy));
        if (magnitude > maximum) {
          maximum = magnitude;
          maxX = x;
          maxY = y;
        }
        if (!reset && (std::abs(vx + 8) > 1 || std::abs(vy - 8) > 1)) {
          ++inaccurate;
          if (x >= 256 && x < w - 256 && y >= 256 && y < h - 256) ++innerBad;
        }
      }
    }
    context->Unmap(staging.Get(), 0);
    std::sort(xs.begin(), xs.end());
    std::sort(ys.begin(), ys.end());
    const float mx = xs[xs.size() / 2], my = ys[ys.size() / 2];
    std::printf("frame=%d reset=%d median=(%.3f,%.3f) max=%.3f at=(%u,%u) inaccurate=%.4f%% innerBad=%zu\n", frame, reset, mx, my, maximum, maxX, maxY, 100.0 * inaccurate / xs.size(), innerBad);
    if (reset ? maximum != 0 : (std::abs(mx + 8) > 1 || std::abs(my - 8) > 1)) return 10;
  }
  double total = 0;
  for (double ms : times) total += ms;
  std::sort(times.begin(), times.end());
  std::printf("PASS %ux%u: downsample + NVOF + densify + full readback avg=%.3f max=%.3f ms (%zu samples)\n", w, h, total / times.size(), times.back(), times.size());
  return 0;
}
