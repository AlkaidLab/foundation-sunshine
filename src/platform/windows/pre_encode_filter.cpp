/**
 * @file src/platform/windows/pre_encode_filter.cpp
 * @brief Vendor-neutral D3D11 pre-encode filter implementations.
 */

#include "pre_encode_filter.h"

#include <filesystem>
#include <mutex>
#include <utility>

#include <d3dcompiler.h>
#include <dxgi.h>

#include "src/logging_severity.h"
#include "rtx_hdr/backend_loader.h"

#if !defined(SUNSHINE_SHADERS_DIR)
  #define SUNSHINE_SHADERS_DIR SUNSHINE_ASSETS_DIR "/shaders/directx"
#endif

namespace platf::dxgi {
  namespace {
    std::mutex external_backend_mutex;

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

    std::string_view
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

    filter_result_t
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

    std::string_view
    truehdr_failure_reason(foundation_truehdr_status_e status, bool during_create) {
      switch (status) {
        case FOUNDATION_TRUEHDR_STATUS_INVALID_ARGUMENT:
          return during_create ? "backend_create_invalid_argument" : "backend_process_invalid_argument";
        case FOUNDATION_TRUEHDR_STATUS_UNSUPPORTED:
          return during_create ? "backend_create_unsupported" : "backend_process_unsupported";
        case FOUNDATION_TRUEHDR_STATUS_RUNTIME_UNAVAILABLE:
          return during_create ? "backend_create_runtime_unavailable" : "backend_process_runtime_unavailable";
        case FOUNDATION_TRUEHDR_STATUS_DEVICE_LOST:
          return during_create ? "backend_create_device_lost" : "backend_process_device_lost";
        case FOUNDATION_TRUEHDR_STATUS_INTERNAL_ERROR:
          return during_create ? "backend_create_internal_error" : "backend_process_internal_error";
        case FOUNDATION_TRUEHDR_STATUS_OK:
          break;
      }
      return during_create ? "backend_create_unknown_error" : "backend_process_unknown_error";
    }

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
                               errors->GetBufferSize()
                             }
                                    : std::string_view { "no compiler diagnostic" });
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

    class external_sdr_to_hdr_filter_t final: public pre_encode_filter_t {
    public:
      external_sdr_to_hdr_filter_t(
        ID3D11Device *device,
        ID3D11DeviceContext *device_context,
        rtx_hdr::backend_loader_t loader,
        pre_encode_filter_config_t config):
          device_ { device },
          device_context_ { device_context },
          loader_ { std::move(loader) },
          config_ { config } {}

      ~external_sdr_to_hdr_filter_t() override {
        destroy_instance();
      }

      bool
      requires_detached_input() const override {
        return true;
      }

      filter_result_t
      process(const gpu_frame_view_t &input) override {
        if (const auto reason = validate_sdr_input(input); !reason.empty()) {
          return { .status = filter_status_e::failed, .frame = {}, .reason = reason };
        }
        if (!ensure_output_and_instance(input.width, input.height)) {
          return { .status = filter_status_e::failed, .frame = {}, .reason = initialization_failure_ };
        }

        foundation_truehdr_status_e status;
        {
          std::lock_guard lock { external_backend_mutex };
          status = loader_.api()->process(
            instance_,
            device_context_,
            input.texture,
            output_texture_.get());
        }
        if (status != FOUNDATION_TRUEHDR_STATUS_OK) {
          return {
            .status = filter_status_e::failed,
            .frame = {},
            .reason = truehdr_failure_reason(status, false),
          };
        }
        return make_scrgb_result(input, output_texture_.get(), output_srv_.get());
      }

      void
      flush() override {
        if (instance_) {
          std::lock_guard lock { external_backend_mutex };
          loader_.api()->flush(instance_);
        }
      }

      std::string_view
      backend_name() const override {
        return "external_sdr_to_hdr";
      }

    private:
      bool
      ensure_output_and_instance(std::uint32_t width, std::uint32_t height) {
        if (instance_ && output_texture_ && width_ == width && height_ == height) {
          return true;
        }
        initialization_failure_ = "backend_initialization_failed";
        destroy_instance();
        output_srv_.reset();
        output_texture_.reset();

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
          initialization_failure_ = "backend_output_allocation_failed";
          return false;
        }
        output_texture_.reset(texture_raw);
        ID3D11ShaderResourceView *srv_raw = nullptr;
        if (FAILED(device_->CreateShaderResourceView(output_texture_.get(), nullptr, &srv_raw))) {
          initialization_failure_ = "backend_output_view_creation_failed";
          output_texture_.reset();
          return false;
        }
        output_srv_.reset(srv_raw);

        foundation_truehdr_config_t config {
          .struct_size = sizeof(foundation_truehdr_config_t),
          .width = width,
          .height = height,
          .contrast = config_.contrast,
          .saturation = config_.saturation,
          .middle_gray_nits = config_.middle_gray_nits,
          .peak_nits = config_.peak_nits,
        };
        foundation_truehdr_status_e status;
        {
          std::lock_guard lock { external_backend_mutex };
          status = loader_.api()->create(device_, &config, &instance_);
        }
        if (status != FOUNDATION_TRUEHDR_STATUS_OK || !instance_) {
          initialization_failure_ = status == FOUNDATION_TRUEHDR_STATUS_OK
                                      ? "backend_create_missing_instance"
                                      : truehdr_failure_reason(status, true);
          instance_ = nullptr;
          output_srv_.reset();
          output_texture_.reset();
          return false;
        }
        width_ = width;
        height_ = height;
        initialization_failure_ = {};
        return true;
      }

      void
      destroy_instance() {
        if (instance_) {
          std::lock_guard lock { external_backend_mutex };
          loader_.api()->destroy(instance_);
          instance_ = nullptr;
        }
        width_ = 0;
        height_ = 0;
      }

      ID3D11Device *device_;
      ID3D11DeviceContext *device_context_;
      rtx_hdr::backend_loader_t loader_;
      pre_encode_filter_config_t config_;
      void *instance_ = nullptr;
      std::string_view initialization_failure_ { "backend_initialization_failed" };
      com_ptr_t<ID3D11Texture2D> output_texture_;
      com_ptr_t<ID3D11ShaderResourceView> output_srv_;
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
    const pre_encode_filter_config_t &config) {
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
      rtx_hdr::backend_loader_t loader;
      if (!loader.load(backend_path)) {
        BOOST_LOG(warning) << "TrueHDR backend unavailable: " << loader.error();
        if (!fallback) {
          BOOST_LOG(error) << "TrueHDR backend and built-in SDR-in-HDR fallback are both unavailable";
          return {};
        }
        return std::make_unique<failover_filter_t>(
          nullptr,
          std::move(fallback),
          loader.error());
      }
      BOOST_LOG(info) << "Loaded external SDR-to-HDR backend; feature creation is deferred until the first frame";
      auto primary = std::make_unique<external_sdr_to_hdr_filter_t>(
        device,
        device_context,
        std::move(loader),
        config);
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
