/**
 * @file src/platform/windows/pre_encode_filter.cpp
 * @brief Vendor-neutral D3D11 pre-encode filter implementations.
 */

#include "pre_encode_filter.h"

#include <filesystem>
#include <utility>

#include <d3dcompiler.h>
#include <dxgi.h>

#include "hdr_backend_factory.h"
#include "pre_encode_filter_helpers.h"
#include "src/logging_severity.h"

#if !defined(SUNSHINE_SHADERS_DIR)
  #define SUNSHINE_SHADERS_DIR SUNSHINE_ASSETS_DIR "/shaders/directx"
#endif

namespace platf::dxgi {
  namespace {
    using namespace filter_detail;

    com_ptr_t<ID3DBlob>
    compile_mock_shader() {
      const auto shader_path =
        std::filesystem::path(SUNSHINE_SHADERS_DIR) / "mock_sdr_to_scrgb_cs.hlsl";
      ID3DBlob *shader_raw = nullptr;
      ID3DBlob *errors_raw = nullptr;
      const auto status = D3DCompileFromFile(
        shader_path.c_str(),
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "main_cs",
        "cs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        &shader_raw,
        &errors_raw);
      com_ptr_t<ID3DBlob> errors { errors_raw };
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to compile pre-encode shader " << shader_path.string()
                         << ": "
                         << (errors ? std::string_view {
                                        static_cast<const char *>(errors->GetBufferPointer()),
                                        errors->GetBufferSize() } :
                                      std::string_view { "no compiler diagnostic" });
        if (shader_raw) {
          shader_raw->Release();
        }
        return {};
      }
      return com_ptr_t<ID3DBlob> { shader_raw };
    }

    class mock_sdr_to_scrgb_filter_t final: public pre_encode_filter_t {
    public:
      mock_sdr_to_scrgb_filter_t(
        ID3D11Device *device,
        ID3D11DeviceContext *device_context,
        com_ptr_t<ID3D11ComputeShader> shader):
          device_ { device },
          device_context_ { device_context },
          shader_ { std::move(shader) } {}

      bool
      requires_detached_input() const override {
        return true;
      }

      filter_result_t
      process(const gpu_frame_view_t &input) override {
        if (const auto reason = validate_sdr_input(input); !reason.empty()) {
          return { .status = filter_status_e::failed, .frame = {}, .reason = reason };
        }
        if (!ensure_output(input.width, input.height)) {
          return { .status = filter_status_e::failed, .frame = {}, .reason = "output_allocation_failed" };
        }

        ID3D11ShaderResourceView *input_srv = input.srv;
        ID3D11UnorderedAccessView *output_uav = output_uav_.get();
        device_context_->CSSetShader(shader_.get(), nullptr, 0);
        device_context_->CSSetShaderResources(0, 1, &input_srv);
        device_context_->CSSetUnorderedAccessViews(0, 1, &output_uav, nullptr);
        device_context_->Dispatch((input.width + 15) / 16, (input.height + 15) / 16, 1);

        ID3D11ShaderResourceView *null_srv = nullptr;
        ID3D11UnorderedAccessView *null_uav = nullptr;
        device_context_->CSSetShaderResources(0, 1, &null_srv);
        device_context_->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);
        device_context_->CSSetShader(nullptr, nullptr, 0);

        return make_scrgb_result(input, output_texture_.get(), output_srv_.get());
      }

      void
      flush() override {
        output_uav_.reset();
        output_srv_.reset();
        output_texture_.reset();
        width_ = 0;
        height_ = 0;
      }

      std::string_view
      backend_name() const override {
        return "gpu_sdr_in_hdr_fallback";
      }

    private:
      bool
      ensure_output(std::uint32_t width, std::uint32_t height) {
        if (output_texture_ && width_ == width && height_ == height) {
          return true;
        }
        flush();

        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

        ID3D11Texture2D *texture_raw = nullptr;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, &texture_raw))) {
          return false;
        }
        output_texture_.reset(texture_raw);

        ID3D11ShaderResourceView *srv_raw = nullptr;
        if (FAILED(device_->CreateShaderResourceView(output_texture_.get(), nullptr, &srv_raw))) {
          flush();
          return false;
        }
        output_srv_.reset(srv_raw);

        ID3D11UnorderedAccessView *uav_raw = nullptr;
        if (FAILED(device_->CreateUnorderedAccessView(output_texture_.get(), nullptr, &uav_raw))) {
          flush();
          return false;
        }
        output_uav_.reset(uav_raw);
        width_ = width;
        height_ = height;
        return true;
      }

      ID3D11Device *device_;
      ID3D11DeviceContext *device_context_;
      com_ptr_t<ID3D11ComputeShader> shader_;
      com_ptr_t<ID3D11Texture2D> output_texture_;
      com_ptr_t<ID3D11ShaderResourceView> output_srv_;
      com_ptr_t<ID3D11UnorderedAccessView> output_uav_;
      std::uint32_t width_ = 0;
      std::uint32_t height_ = 0;
    };

    class failover_filter_t final: public pre_encode_filter_t {
    public:
      failover_filter_t(
        std::unique_ptr<pre_encode_filter_t> primary,
        std::unique_ptr<pre_encode_filter_t> fallback,
        std::string initial_failure = {}):
          primary_ { std::move(primary) },
          fallback_ { std::move(fallback) },
          degraded_ { !primary_ },
          failure_reason_ { std::move(initial_failure) } {}

      bool
      requires_detached_input() const override {
        return true;
      }

      filter_result_t
      process(const gpu_frame_view_t &input) override {
        if (!degraded_ && primary_) {
          auto result = primary_->process(input);
          if (result.status != filter_status_e::failed) {
            return result;
          }
          failure_reason_.assign(result.reason);
          primary_->flush();
          primary_.reset();
          degraded_ = true;
        }
        return fallback_->process(input);
      }

      void
      flush() override {
        if (primary_) {
          primary_->flush();
        }
        fallback_->flush();
      }

      std::string_view
      backend_name() const override {
        return degraded_ ? fallback_->backend_name() : primary_->backend_name();
      }

      bool
      degraded() const override {
        return degraded_;
      }

      std::string_view
      failure_reason() const override {
        return failure_reason_;
      }

    private:
      std::unique_ptr<pre_encode_filter_t> primary_;
      std::unique_ptr<pre_encode_filter_t> fallback_;
      bool degraded_ = false;
      std::string failure_reason_;
    };

    std::unique_ptr<pre_encode_filter_t>
    make_mock_filter(ID3D11Device *device, ID3D11DeviceContext *device_context) {
      auto shader_blob = compile_mock_shader();
      if (!shader_blob) {
        return {};
      }
      ID3D11ComputeShader *shader_raw = nullptr;
      if (FAILED(device->CreateComputeShader(
            shader_blob->GetBufferPointer(),
            shader_blob->GetBufferSize(),
            nullptr,
            &shader_raw))) {
        return {};
      }
      return std::make_unique<mock_sdr_to_scrgb_filter_t>(
        device,
        device_context,
        com_ptr_t<ID3D11ComputeShader> { shader_raw });
    }
  }  // namespace

  std::unique_ptr<pre_encode_filter_t>
  make_pre_encode_filter(
    pre_encode_filter_e kind,
    ID3D11Device *device,
    ID3D11DeviceContext *device_context,
    const std::filesystem::path &backend_path,
    const pre_encode_filter_config_t &config,
    std::string_view backend_id) {
    if (kind == pre_encode_filter_e::none) {
      return {};
    }
    if (!device || !device_context) {
      BOOST_LOG(error) << "Cannot create pre-encode filter without a D3D11 device and immediate context";
      return {};
    }
    if (kind == pre_encode_filter_e::mock_sdr_to_scrgb) {
      return make_mock_filter(device, device_context);
    }
    if (kind == pre_encode_filter_e::external_sdr_to_hdr) {
      auto fallback = make_mock_filter(device, device_context);
      std::string failure;
      auto primary = make_hdr_backend(backend_id, device, device_context, backend_path, config, failure);
      if (!primary) {
        BOOST_LOG(warning) << "HDR enhancement backend unavailable: " << failure;
        if (!fallback) {
          BOOST_LOG(error) << "TrueHDR backend and built-in SDR-in-HDR fallback are both unavailable";
          return {};
        }
        return std::make_unique<failover_filter_t>(
          nullptr,
          std::move(fallback),
          failure);
      }
      BOOST_LOG(info) << "Loaded external SDR-to-HDR backend; feature creation is deferred until the first frame";
      // The optional fallback must never gate the vendor backend. A missing
      // fallback asset reduces resilience for this session, but the primary
      // backend can still process frames normally.
      if (!fallback) {
        return primary;
      }
      return std::make_unique<failover_filter_t>(std::move(primary), std::move(fallback));
    }
    BOOST_LOG(error) << "Unknown pre-encode filter kind: " << static_cast<int>(kind);
    return {};
  }
}  // namespace platf::dxgi
