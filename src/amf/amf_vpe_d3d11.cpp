/**
 * @file src/amf/amf_vpe_d3d11.cpp
 * @brief Opt-in VPE preference with a bounded, synchronous GPU output handoff.
 */
#include "amf_vpe_d3d11.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <d3d11_1.h>
#include <thread>
#include <wrl/client.h>

#include <AMF/components/VideoConverter.h>
#include <AMF/core/Factory.h>

#include "src/logging.h"

namespace amf {
  using Microsoft::WRL::ComPtr;

  struct amf_vpe_d3d11::impl {
    HMODULE library = nullptr;
    AMFContextPtr context;
    AMFComponentPtr converter;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> immediate;
    ComPtr<ID3D11DeviceContext1> immediate1;
    ComPtr<ID3DDeviceContextState> private_state;
    ComPtr<ID3D11Query> completion;
    ComPtr<ID3D11ComputeShader> hlg_prepare_shader;
    ComPtr<ID3D11Texture2D> hlg_input;
    ComPtr<ID3D11UnorderedAccessView> hlg_input_uav;
    ComPtr<ID3D11Buffer> hlg_parameters;
    video::sunshine_colorspace_t colorspace;
    int width, height, active_width, active_height, offset_x, offset_y;
    float hlg_peak_nits = 1000.0f;
    DXGI_FORMAT input_format = DXGI_FORMAT_UNKNOWN;
    UINT input_width = 0, input_height = 0;
    bool ready = false;
    std::string failure;
    uint64_t frames = 0;
    double elapsed_ms = 0;

    struct context_state_guard {
      ID3D11DeviceContext1 *immediate;
      ComPtr<ID3DDeviceContextState> previous;
      context_state_guard(ID3D11DeviceContext1 *immediate, ID3DDeviceContextState *state):
          immediate(immediate) {
        if (immediate) immediate->SwapDeviceContextState(state, &previous);
      }
      ~context_state_guard() {
        if (immediate) immediate->SwapDeviceContextState(previous.Get(), nullptr);
      }
    };

    ~impl() {
      context_state_guard guard(immediate1.Get(), private_state.Get());
      // Release AMF interfaces before unloading the runtime that implements them.
      if (converter) converter->Terminate();
      converter = nullptr;
      if (context) context->Terminate();
      context = nullptr;
      if (library) FreeLibrary(library);
    }

    bool
    fail(const std::string &reason) {
      failure = reason;
      BOOST_LOG(info) << "AMF VPE experiment: falling back to D3D11 shaders (" << reason << ")";
      // Local AMF surfaces in convert() still have to Release() through this DLL.
      // Keep the context and runtime loaded until the owner destroys the backend.
      ready = false;
      if (converter) converter->Terminate();
      return false;
    }

    bool
    initialize() {
      ComPtr<IDXGIDevice> dxgi_device;
      ComPtr<IDXGIAdapter> adapter;
      DXGI_ADAPTER_DESC desc {};
      if (FAILED(device.As(&dxgi_device)) || FAILED(dxgi_device->GetAdapter(&adapter)) ||
          FAILED(adapter->GetDesc(&desc)) || desc.VendorId != 0x1002) {
        return fail("non_amd_device");
      }
      ComPtr<ID3D11Device1> device1;
      const auto level = device->GetFeatureLevel();
      if (FAILED(device.As(&device1)) || FAILED(immediate.As(&immediate1)) ||
          FAILED(device1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION,
            __uuidof(ID3D11Device), nullptr, &private_state))) {
        immediate1.Reset();
        return fail("context_state_isolation_unavailable");
      }
      // SDK initialization and compute fallback must not disturb the caller's shader state.
      context_state_guard guard(immediate1.Get(), private_state.Get());
      library = LoadLibraryA(AMF_DLL_NAMEA);
      if (!library) return fail("runtime_unavailable");
      auto init = reinterpret_cast<AMFInit_Fn>(GetProcAddress(library, AMF_INIT_FUNCTION_NAME));
      auto query = reinterpret_cast<AMFQueryVersion_Fn>(GetProcAddress(library, AMF_QUERY_VERSION_FUNCTION_NAME));
      amf_uint64 version = 0;
      AMFFactory *factory = nullptr;
      if (!init || !query || query(&version) != AMF_OK || version < AMF_MAKE_FULL_VERSION(1, 5, 3, 0)) {
        return fail("runtime_requires_1_5_3");
      }
      if (init(AMF_FULL_VERSION, &factory) != AMF_OK || !factory ||
          factory->CreateContext(&context) != AMF_OK || context->InitDX11(device.Get()) != AMF_OK ||
          factory->CreateComponent(context, AMFVideoConverter, &converter) != AMF_OK) {
        return fail("converter_unavailable");
      }
      AMFCapsPtr caps;
      amf_bool vpe_available = false;
      if (converter->GetCaps(&caps) != AMF_OK || !caps ||
          caps->GetProperty(AMF_VIDEO_CONVERTER_CAP_VPE_AVAILABILITY, &vpe_available) != AMF_OK || !vpe_available) {
        return fail("vpe_unavailable");
      }
      D3D11_QUERY_DESC query_desc { D3D11_QUERY_EVENT, 0 };
      if (FAILED(device->CreateQuery(&query_desc, &completion))) return fail("sync_unavailable");
      ready = true;
      BOOST_LOG(info) << "AMF VPE experiment: hardware available; requesting VPE_PREFERRED. "
                         "The driver may use compute for unsupported configurations. "
                         "This experiment waits for completion and copies the GPU output into the encoder texture.";
      return true;
    }

    template <class T>
    bool
    set(const wchar_t *name, const T &value) {
      return converter->SetProperty(name, value) == AMF_OK;
    }

    bool
    configure(const D3D11_TEXTURE2D_DESC &input, bool linear_fp16) {
      const bool pq = video::colorspace_is_pq(colorspace);
      const bool hlg = video::colorspace_is_hlg(colorspace);
      const bool hdr = pq || hlg;
      if (hlg && (!std::isfinite(hlg_peak_nits) || hlg_peak_nits <= 0 || hlg_peak_nits > 10000.0f)) {
        return fail("hlg_peak_unsupported");
      }
      if (hdr) {
        // Preserve the same 0..10000-nit range as our PQ shader. AMF's default
        // input luminance limits otherwise lift black and clip above 1000 nits.
        // This is converter input metadata, not the encoded display metadata.
        AMFHDRMetadata limits {};
        // HLG's prepass produces normalized scene-linear Rec.2020. A generic
        // linear AMF input with a 10000-nit nominal range gives its HLG OETF
        // that normalized signal without substituting AMF's display OOTF.
        const float peak = 10000.0f;
        limits.maxMasteringLuminance = static_cast<amf_uint32>(std::lround(peak * 10000.0f));
        limits.maxContentLightLevel = static_cast<amf_uint16>(std::lround(peak));
        limits.maxFrameAverageLightLevel = limits.maxContentLightLevel;
        AMFBufferPtr metadata;
        if (context->AllocBuffer(AMF_MEMORY_HOST, sizeof(limits), &metadata) != AMF_OK || !metadata) {
          return fail("hdr_input_metadata_allocation_failed");
        }
        std::memcpy(metadata->GetNative(), &limits, sizeof(limits));
        if (!set(AMF_VIDEO_CONVERTER_INPUT_HDR_METADATA, metadata)) return fail("hdr_input_metadata_unsupported");
        if (hlg && !set(AMF_VIDEO_CONVERTER_OUTPUT_HDR_METADATA, metadata)) return fail("hlg_output_metadata_unsupported");
      }
      AMF_SURFACE_FORMAT format;
      switch (input.Format) {
        case DXGI_FORMAT_B8G8R8A8_UNORM:
          format = AMF_SURFACE_BGRA;
          break;
        case DXGI_FORMAT_R8G8B8A8_UNORM:
          format = AMF_SURFACE_RGBA;
          break;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
          format = AMF_SURFACE_RGBA_F16;
          break;
        default:
          return fail("input_format_unsupported");
      }
      if (hdr != linear_fp16) return fail("input_color_domain_unsupported");
      if (hlg) {
        if (!hlg_prepare_shader) return fail("hlg_prepare_shader_unavailable");
        D3D11_TEXTURE2D_DESC prepared = input;
        prepared.Usage = D3D11_USAGE_DEFAULT;
        prepared.CPUAccessFlags = 0;
        prepared.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        prepared.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
        if (FAILED(device->CreateTexture2D(&prepared, nullptr, &hlg_input)) ||
            FAILED(device->CreateUnorderedAccessView(hlg_input.Get(), nullptr, &hlg_input_uav))) {
          return fail("hlg_prepare_texture_unsupported");
        }
        const float parameters[4] { hlg_peak_nits, video::hlg_system_gamma(hlg_peak_nits), 0, 0 };
        D3D11_BUFFER_DESC buffer {};
        buffer.ByteWidth = sizeof(parameters);
        buffer.Usage = D3D11_USAGE_IMMUTABLE;
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA contents { parameters, 0, 0 };
        if (FAILED(device->CreateBuffer(&buffer, &contents, &hlg_parameters))) return fail("hlg_parameters_unavailable");
        BOOST_LOG(info) << "AMF VPE experiment: HLG scene-linear prepass, peak=" << hlg_peak_nits
                        << " nits, system gamma=" << parameters[1] << "; scaling/conversion requested from AMF";
      }
      if (!hdr && colorspace.colorspace != video::colorspace_e::rec601 && colorspace.colorspace != video::colorspace_e::rec709) {
        return fail("sdr_color_profile_unvalidated");
      }
      auto profile = colorspace.full_range ? AMF_VIDEO_CONVERTER_COLOR_PROFILE_FULL_709 : AMF_VIDEO_CONVERTER_COLOR_PROFILE_709;
      if (hdr)
        profile = colorspace.full_range ? AMF_VIDEO_CONVERTER_COLOR_PROFILE_FULL_2020 : AMF_VIDEO_CONVERTER_COLOR_PROFILE_2020;
      else if (colorspace.colorspace == video::colorspace_e::rec601)
        profile = colorspace.full_range ? AMF_VIDEO_CONVERTER_COLOR_PROFILE_FULL_601 : AMF_VIDEO_CONVERTER_COLOR_PROFILE_601;
      // Describe the input explicitly so driver HDR defaults cannot tone-map an HDR stream to SDR.
      auto output_primaries = AMF_COLOR_PRIMARIES_BT709;
      auto output_transfer = AMF_COLOR_TRANSFER_CHARACTERISTIC_BT709;
      auto output_matrix = AMF_COLOR_MATRIX_COEFF_BT_709;
      if (hdr) {
        output_primaries = AMF_COLOR_PRIMARIES_BT2020;
        output_transfer = hlg ? AMF_COLOR_TRANSFER_CHARACTERISTIC_ARIB_STD_B67 : AMF_COLOR_TRANSFER_CHARACTERISTIC_SMPTE2084;
        output_matrix = AMF_COLOR_MATRIX_COEFF_BT_2020_NCL;
      }
      else if (colorspace.colorspace == video::colorspace_e::rec601) {
        output_primaries = AMF_COLOR_PRIMARIES_SMPTE170M;
        output_matrix = AMF_COLOR_MATRIX_COEFF_BT_601;
      }
      const auto input_primaries = hlg ? AMF_COLOR_PRIMARIES_BT2020 : pq ? AMF_COLOR_PRIMARIES_CCCS :
                                                                           output_primaries;
      if (!set(AMF_VIDEO_CONVERTER_ACCELERATION_TYPE, (amf_int64) AMF_VIDEO_CONVERTER_ACCELERATION_VPE_PREFERRED) ||
          !set(AMF_VIDEO_CONVERTER_MEMORY_TYPE, (amf_int64) AMF_MEMORY_DX11) ||
          !set(AMF_VIDEO_CONVERTER_OUTPUT_FORMAT, (amf_int64) (hdr ? AMF_SURFACE_P010 : AMF_SURFACE_NV12)) ||
          !set(AMF_VIDEO_CONVERTER_OUTPUT_SIZE, AMFConstructSize(width, height)) ||
          !set(AMF_VIDEO_CONVERTER_OUTPUT_RECT, AMFConstructRect(offset_x, offset_y, offset_x + active_width, offset_y + active_height)) ||
          !set(AMF_VIDEO_CONVERTER_FILL, true) ||
          !set(AMF_VIDEO_CONVERTER_FILL_COLOR, AMFConstructColor(0, 0, 0, 255)) ||
          !set(AMF_VIDEO_CONVERTER_SCALE, (amf_int64) AMF_VIDEO_CONVERTER_SCALE_BICUBIC) ||
          !set(AMF_VIDEO_CONVERTER_COLOR_PROFILE, (amf_int64) profile) ||
          !set(AMF_VIDEO_CONVERTER_USE_DECODER_HDR_METADATA, false) ||
          !set(AMF_VIDEO_CONVERTER_INPUT_TRANSFER_CHARACTERISTIC, (amf_int64) (hdr ? AMF_COLOR_TRANSFER_CHARACTERISTIC_LINEAR : AMF_COLOR_TRANSFER_CHARACTERISTIC_BT709)) ||
          // PQ reads Windows scRGB (1.0 = 80 nits). HLG reads the prepass's scene-linear Rec.2020.
          !set(AMF_VIDEO_CONVERTER_INPUT_COLOR_PRIMARIES, (amf_int64) input_primaries) ||
          !set(AMF_VIDEO_CONVERTER_INPUT_COLOR_MATRIX_COEFF, (amf_int64) AMF_COLOR_MATRIX_COEFF_GBR) ||
          !set(AMF_VIDEO_CONVERTER_INPUT_COLOR_RANGE, (amf_int64) AMF_COLOR_RANGE_FULL) ||
          !set(AMF_VIDEO_CONVERTER_INPUT_TONEMAPPING, (amf_int64) AMF_VIDEO_CONVERTER_TONEMAPPING_COPY) ||
          !set(AMF_VIDEO_CONVERTER_OUTPUT_COLOR_PRIMARIES, (amf_int64) output_primaries) ||
          !set(AMF_VIDEO_CONVERTER_OUTPUT_TRANSFER_CHARACTERISTIC, (amf_int64) output_transfer) ||
          !set(AMF_VIDEO_CONVERTER_OUTPUT_COLOR_MATRIX_COEFF, (amf_int64) output_matrix) ||
          !set(AMF_VIDEO_CONVERTER_OUTPUT_COLOR_RANGE, (amf_int64) (colorspace.full_range ? AMF_COLOR_RANGE_FULL : AMF_COLOR_RANGE_STUDIO)) ||
          converter->Init(format, input.Width, input.Height) != AMF_OK) {
        return fail("configuration_unsupported");
      }
      input_format = input.Format;
      input_width = input.Width;
      input_height = input.Height;
      return true;
    }

    bool
    finish_dx11() {
      immediate->End(completion.Get());
      immediate->Flush();
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
      for (;;) {
        const auto hr = immediate->GetData(completion.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_OK) return true;
        if (FAILED(hr) || std::chrono::steady_clock::now() >= deadline) return fail("gpu_completion_failed");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }

    bool
    convert(ID3D11Texture2D *input, ID3D11Texture2D *output, bool linear_fp16) {
      if (!ready) return false;
      context_state_guard guard(immediate1.Get(), private_state.Get());
      D3D11_TEXTURE2D_DESC in {}, out {};
      input->GetDesc(&in);
      output->GetDesc(&out);
      const bool hdr = video::colorspace_is_pq(colorspace) || video::colorspace_is_hlg(colorspace);
      const auto expected_output = hdr ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
      if (in.ArraySize != 1 || in.MipLevels != 1 || in.SampleDesc.Count != 1 || out.ArraySize != 1 ||
          out.Format != expected_output || out.Width < (UINT) width || out.Height < (UINT) height ||
          !(in.MiscFlags & (D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX))) {
        return fail("texture_not_shareable_or_layout_unsupported");
      }
      if (input_format == DXGI_FORMAT_UNKNOWN && !configure(in, linear_fp16)) return false;
      if (input_format != in.Format || input_width != in.Width || input_height != in.Height ||
          hdr != linear_fp16) return fail("input_layout_or_domain_changed");
      const auto start = std::chrono::steady_clock::now();
      if (hlg_input) {
        ComPtr<ID3D11ShaderResourceView> source_view;
        if (FAILED(device->CreateShaderResourceView(input, nullptr, &source_view))) return fail("hlg_input_view_unsupported");
        auto *srv = source_view.Get();
        auto *uav = hlg_input_uav.Get();
        auto *parameters = hlg_parameters.Get();
        immediate->CSSetShader(hlg_prepare_shader.Get(), nullptr, 0);
        immediate->CSSetShaderResources(0, 1, &srv);
        immediate->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        immediate->CSSetConstantBuffers(3, 1, &parameters);
        immediate->Dispatch((in.Width + 15) / 16, (in.Height + 15) / 16, 1);
        srv = nullptr;
        uav = nullptr;
        immediate->CSSetShaderResources(0, 1, &srv);
        immediate->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        immediate->CSSetShader(nullptr, nullptr, 0);
      }
      // Finish cursor/filter writes before another GPU engine reads the capture resource.
      if (!finish_dx11()) return false;
      AMFSurfacePtr source;
      if (context->CreateSurfaceFromDX11Native(hlg_input ? hlg_input.Get() : input, &source, nullptr) != AMF_OK) return fail("input_import_failed");
      source->SetPts((amf_pts) frames);
      if (converter->SubmitInput(source) != AMF_OK) return fail("submit_failed");
      AMFDataPtr data;
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
      while (!data) {
        auto res = converter->QueryOutput(&data);
        if (res != AMF_OK && res != AMF_REPEAT && res != AMF_NEED_MORE_INPUT) return fail("query_failed");
        if (!data) {
          if (std::chrono::steady_clock::now() >= deadline) return fail("query_timeout");
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      }
      AMFSurfacePtr converted(data);
      // Let AMF perform the cross-engine/API interop before native DX11 access.
      if (!converted || converted->Convert(AMF_MEMORY_DX11) != AMF_OK) return fail("output_interop_failed");
      auto *plane = converted->GetPlaneAt(0);
      auto *texture = plane ? static_cast<ID3D11Texture2D *>(plane->GetNative()) : nullptr;
      if (!texture) return fail("output_texture_missing");
      D3D11_TEXTURE2D_DESC result {};
      texture->GetDesc(&result);
      if (result.Format != out.Format || result.ArraySize != 1 || result.Width < (UINT) width || result.Height < (UINT) height) {
        return fail("output_layout_unsupported");
      }
      D3D11_BOX box { 0, 0, 0, (UINT) width, (UINT) height, 1 };
      immediate->CopySubresourceRegion(output, 0, 0, 0, 0, texture, 0, &box);
      // Keep both AMF surfaces alive, and keep the capture slot locked, until the GPU copy retires.
      if (!finish_dx11()) return false;
      elapsed_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      if (++frames % 120 == 0) {
        BOOST_LOG(info) << "AMF VPE experiment: VPE_PREFERRED frames=" << frames
                        << " average synchronous conversion+GPU copy=" << elapsed_ms / frames
                        << " ms (driver compute fallback remains possible)";
      }
      return true;
    }
  };

  amf_vpe_d3d11::amf_vpe_d3d11(ID3D11Device *device, ID3D11DeviceContext *immediate,
    const video::sunshine_colorspace_t &colorspace, int width, int height,
    int active_width, int active_height, int offset_x, int offset_y, float hlg_peak_nits,
    ID3D11ComputeShader *hlg_prepare_shader):
      state(std::make_unique<impl>()) {
    state->device = device;
    state->immediate = immediate;
    state->colorspace = colorspace;
    state->width = width;
    state->height = height;
    state->active_width = active_width;
    state->active_height = active_height;
    state->offset_x = offset_x;
    state->offset_y = offset_y;
    state->hlg_peak_nits = hlg_peak_nits;
    state->hlg_prepare_shader = hlg_prepare_shader;
    state->initialize();
  }

  amf_vpe_d3d11::~amf_vpe_d3d11() = default;
  bool
  amf_vpe_d3d11::available() const { return state->ready; }
  const std::string &
  amf_vpe_d3d11::failure_reason() const { return state->failure; }
  bool
  amf_vpe_d3d11::convert(ID3D11Texture2D *input, ID3D11Texture2D *output, bool linear_fp16) {
    return state->convert(input, output, linear_fp16);
  }
}  // namespace amf
