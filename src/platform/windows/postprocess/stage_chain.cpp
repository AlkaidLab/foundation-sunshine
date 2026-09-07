/**
 * @file src/platform/windows/postprocess/stage_chain.cpp
 * @brief Chain executor and v2 stage driver (see stage_chain.h).
 */
#include "stage_chain.h"

#include <algorithm>
#include <mutex>
#include <utility>

#include "src/logging.h"
#include "src/platform/frame_contract.h"

namespace platf::dxgi::postprocess {
  // The ABI mirrors the host-side frame contract enums by value; a drift here
  // would silently corrupt every stage's domain negotiation.
  static_assert(static_cast<std::uint32_t>(platf::frame_domain_e::unknown) == FOUNDATION_STAGE_DOMAIN_UNKNOWN);
  static_assert(static_cast<std::uint32_t>(platf::frame_domain_e::sdr_rec709) == FOUNDATION_STAGE_DOMAIN_SDR_REC709);
  static_assert(static_cast<std::uint32_t>(platf::frame_domain_e::linear_scrgb) == FOUNDATION_STAGE_DOMAIN_LINEAR_SCRGB);
  static_assert(static_cast<std::uint32_t>(platf::frame_domain_e::pq_bt2020) == FOUNDATION_STAGE_DOMAIN_PQ_BT2020);
  static_assert(static_cast<std::uint32_t>(platf::frame_domain_e::hlg_bt2020) == FOUNDATION_STAGE_DOMAIN_HLG_BT2020);
  static_assert(static_cast<std::uint32_t>(platf::pixel_encoding_class_e::automatic) == FOUNDATION_STAGE_ENCODING_AUTOMATIC);
  static_assert(static_cast<std::uint32_t>(platf::pixel_encoding_class_e::unorm8) == FOUNDATION_STAGE_ENCODING_UNORM8);
  static_assert(static_cast<std::uint32_t>(platf::pixel_encoding_class_e::float16) == FOUNDATION_STAGE_ENCODING_FLOAT16);

  // ------------------------------------------------------------------
  // chain_filter_t
  // ------------------------------------------------------------------
  chain_filter_t::chain_filter_t(std::vector<std::unique_ptr<pre_encode_filter_t>> stages) {
    slots_.reserve(stages.size());
    for (auto &stage: stages) {
      stage_state_t state;
      state.name = std::string { stage->backend_name() };
      slots_.push_back({ std::move(stage), std::move(state) });
    }
  }

  bool
  chain_filter_t::requires_detached_input() const {
    return true;
  }

  filter_result_t
  chain_filter_t::process(const gpu_frame_view_t &input) {
    gpu_frame_view_t frame = input;
    for (auto &slot: slots_) {
      if (!slot.filter) {
        continue;  // bypassed earlier this session
      }
      auto result = slot.filter->process(frame);
      if (result.status == filter_status_e::failed) {
        // R8 bypass: drop this stage for the session and let the frame flow
        // on. Neighboring stages enforce their own input contracts, so a
        // bypass that breaks the domain chain fails through naturally.
        slot.state.state = "bypassed";
        slot.state.failure_reason.assign(result.reason);
        failure_reason_.assign(result.reason);
        BOOST_LOG(warning) << "Post-process stage '" << slot.state.name
                           << "' bypassed for this session: " << result.reason;
        slot.filter->flush();
        slot.filter.reset();
        ++bypassed_;
        continue;
      }
      if (result.status == filter_status_e::ready && result.frame.texture) {
        frame = result.frame;
      }
    }

    if (bypassed_ >= slots_.size()) {
      // The chain is empty; propagate the last child failure so the outer
      // failover reports and degrades with the child's reason, exactly as the
      // single-filter path did before chains existed.
      return { .status = filter_status_e::failed, .frame = {}, .reason = failure_reason_ };
    }
    return {
      .status = filter_status_e::ready,
      .frame = frame,
      .reason = {},
    };
  }

  void
  chain_filter_t::flush() {
    for (auto &slot: slots_) {
      if (slot.filter) {
        slot.filter->flush();
      }
    }
  }

  std::string_view
  chain_filter_t::backend_name() const {
    // A single-stage chain is transparent (the rtx_hdr migration must keep
    // reporting the child's name); longer chains report as a chain.
    const pre_encode_filter_t *sole = nullptr;
    std::uint32_t live = 0;
    for (const auto &slot: slots_) {
      if (slot.filter) {
        sole = slot.filter.get();
        ++live;
      }
    }
    if (live == 1 && sole) {
      return sole->backend_name();
    }
    return "postprocess_chain";
  }

  bool
  chain_filter_t::degraded() const {
    return bypassed_ > 0;
  }

  std::string_view
  chain_filter_t::failure_reason() const {
    return failure_reason_;
  }

  std::vector<stage_state_t>
  chain_filter_t::postprocess_stage_states() const {
    std::vector<stage_state_t> states;
    states.reserve(slots_.size());
    for (const auto &slot: slots_) {
      states.push_back(slot.state);
    }
    return states;
  }

  const std::vector<pre_encode_filter_t *>
  chain_filter_t::active_stages() const {
    std::vector<pre_encode_filter_t *> stages;
    for (const auto &slot: slots_) {
      if (slot.filter) {
        stages.push_back(slot.filter.get());
      }
    }
    return stages;
  }

  std::unique_ptr<pre_encode_filter_t>
  make_stage_chain(std::vector<std::unique_ptr<pre_encode_filter_t>> stages) {
    if (stages.empty()) {
      return {};
    }
    for (const auto &stage: stages) {
      if (!stage) {
        return {};
      }
    }
    return std::make_unique<chain_filter_t>(std::move(stages));
  }

  // ------------------------------------------------------------------
  // v2 DLL stage driver
  // ------------------------------------------------------------------
  namespace {
    std::mutex dll_stage_mutex;

    std::string_view
    stage_failure_reason(foundation_stage_status_e status, bool during_create) {
      switch (status) {
        case FOUNDATION_STAGE_STATUS_INVALID_ARGUMENT:
          return during_create ? "stage_create_invalid_argument" : "stage_process_invalid_argument";
        case FOUNDATION_STAGE_STATUS_UNSUPPORTED:
          return during_create ? "stage_create_unsupported" : "stage_process_unsupported";
        case FOUNDATION_STAGE_STATUS_RUNTIME_UNAVAILABLE:
          return during_create ? "stage_create_runtime_unavailable" : "stage_process_runtime_unavailable";
        case FOUNDATION_STAGE_STATUS_DEVICE_LOST:
          return during_create ? "stage_create_device_lost" : "stage_process_device_lost";
        case FOUNDATION_STAGE_STATUS_INTERNAL_ERROR:
          return during_create ? "stage_create_internal_error" : "stage_process_internal_error";
        case FOUNDATION_STAGE_STATUS_OK:
          break;
      }
      return during_create ? "stage_create_unknown_error" : "stage_process_unknown_error";
    }

    DXGI_FORMAT
    encoding_host_format(std::uint32_t encoding) {
      switch (encoding) {
        case FOUNDATION_STAGE_ENCODING_FLOAT16:
          return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case FOUNDATION_STAGE_ENCODING_UNORM8:
          return DXGI_FORMAT_B8G8R8A8_UNORM;
        default:
          return DXGI_FORMAT_UNKNOWN;
      }
    }

    std::string_view
    validate_stage_input(const gpu_frame_view_t &input, const foundation_stage_caps_t *caps) {
      if (!input.texture || !input.srv || input.width == 0 || input.height == 0) {
        return "invalid_input";
      }
      if (input.semantic.borrowed) {
        return "input_contract_mismatch";
      }
      if (static_cast<std::uint32_t>(input.semantic.domain) != caps->input_domain ||
          static_cast<std::uint32_t>(input.semantic.encoding) != caps->input_encoding) {
        return "input_contract_mismatch";
      }
      const auto expected = encoding_host_format(caps->input_encoding);
      if (expected != DXGI_FORMAT_UNKNOWN && input.format != expected &&
          // unorm8 stages also accept the X8 padding variant of BGRA8
          !(caps->input_encoding == FOUNDATION_STAGE_ENCODING_UNORM8 && input.format == DXGI_FORMAT_B8G8R8X8_UNORM) &&
          !(caps->input_encoding == FOUNDATION_STAGE_ENCODING_UNORM8 && input.format == DXGI_FORMAT_R8G8B8A8_UNORM)) {
        return "unsupported_format";
      }
      return {};
    }

    class dll_stage_filter_t final: public pre_encode_filter_t {
    public:
      dll_stage_filter_t(
        ID3D11Device *device,
        ID3D11DeviceContext *device_context,
        rtx_hdr::backend_loader_t loader,
        float scale_hint,
        const std::string &params_json):
          device_ { device },
          device_context_ { device_context },
          loader_ { std::move(loader) },
          scale_hint_ { scale_hint },
          params_json_ { params_json } {}

      ~dll_stage_filter_t() override {
        destroy_instance();
        release_outputs();
      }

      bool
      requires_detached_input() const override {
        return true;
      }

      filter_result_t
      process(const gpu_frame_view_t &input) override {
        const auto *caps = loader_.stage_api()->caps();
        if (const auto reason = validate_stage_input(input, caps); !reason.empty()) {
          return { .status = filter_status_e::failed, .frame = {}, .reason = reason };
        }
        if (!ensure_outputs_and_instance(input.width, input.height)) {
          return { .status = filter_status_e::failed, .frame = {}, .reason = initialization_failure_ };
        }

        foundation_stage_frame_t abi_input {
          .texture = input.texture,
          .srv = input.srv,
          .dxgi_format = input.format,
          .width = input.width,
          .height = input.height,
          .domain = caps->input_domain,
          .encoding = caps->input_encoding,
          .reference_white_nits = input.semantic.reference_white_nits,
          .frame_type = FOUNDATION_STAGE_FRAME_REAL,
          .source_generation = input.semantic.source_generation,
        };
        std::uint32_t out_count = 0;
        foundation_stage_status_e status;
        {
          std::lock_guard lock { dll_stage_mutex };
          status = loader_.stage_api()->process(instance_, &abi_input, outputs_.data(), &out_count);
        }
        if (status != FOUNDATION_STAGE_STATUS_OK || out_count == 0) {
          return {
            .status = filter_status_e::failed,
            .frame = {},
            .reason = status == FOUNDATION_STAGE_STATUS_OK ? "stage_process_no_output" : stage_failure_reason(status, false),
          };
        }

        // Single-output consumption: the pre_encode_filter_t contract carries
        // one frame. Temporal stages are refused at make time, so the first
        // output must be the real frame, at the host-allocated size. The
        // domain/encoding/texture/srv fields were prefilled by the host — a
        // DLL that overwrites any of them (unknown domain value, foreign
        // texture pointer) is rejected rather than propagated downstream.
        if (outputs_[0].frame_type != FOUNDATION_STAGE_FRAME_REAL ||
            outputs_[0].width != output_width_ || outputs_[0].height != output_height_ ||
            outputs_[0].domain != caps->output_domain ||
            outputs_[0].encoding != caps->output_encoding ||
            outputs_[0].texture != static_cast<void *>(textures_.front()) ||
            outputs_[0].srv != static_cast<void *>(srvs_.front())) {
          return { .status = filter_status_e::failed, .frame = {}, .reason = "stage_output_metadata_mismatch" };
        }
        return make_stage_result(outputs_[0]);
      }

      void
      flush() override {
        if (instance_) {
          std::lock_guard lock { dll_stage_mutex };
          loader_.stage_api()->flush(instance_);
        }
      }

      std::string_view
      backend_name() const override {
        return loader_.stage_api()->caps()->name;
      }

    private:
      filter_result_t
      make_stage_result(const foundation_stage_frame_t &frame) {
        gpu_frame_view_t view;
        view.texture = static_cast<ID3D11Texture2D *>(frame.texture);
        view.srv = static_cast<ID3D11ShaderResourceView *>(frame.srv);
        view.format = static_cast<DXGI_FORMAT>(frame.dxgi_format);
        view.width = frame.width;
        view.height = frame.height;
        view.semantic.domain = static_cast<platf::frame_domain_e>(frame.domain);
        view.semantic.encoding = static_cast<platf::pixel_encoding_class_e>(frame.encoding);
        view.semantic.reference_white_nits = frame.reference_white_nits;
        view.semantic.borrowed = false;
        view.semantic.source_generation = frame.source_generation;
        return {
          .status = filter_status_e::ready,
          .frame = view,
          .reason = {},
        };
      }

      bool
      ensure_outputs_and_instance(std::uint32_t width, std::uint32_t height) {
        if (instance_ && !outputs_.empty() && width_ == width && height_ == height) {
          return true;
        }
        initialization_failure_ = "stage_initialization_failed";
        destroy_instance();
        release_outputs();

        const auto *caps = loader_.stage_api()->caps();
        output_width_ = static_cast<std::uint32_t>(static_cast<float>(width) * scale_hint_ + 0.5f);
        output_height_ = static_cast<std::uint32_t>(static_cast<float>(height) * scale_hint_ + 0.5f);
        if (output_width_ == 0 || output_height_ == 0) {
          initialization_failure_ = "stage_output_size_invalid";
          return false;
        }

        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = output_width_;
        desc.Height = output_height_;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = encoding_host_format(caps->output_encoding);
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        if (desc.Format == DXGI_FORMAT_UNKNOWN) {
          initialization_failure_ = "stage_output_encoding_unsupported";
          return false;
        }

        outputs_.resize(caps->max_frames_out);
        for (auto &output: outputs_) {
          ID3D11Texture2D *texture_raw = nullptr;
          if (FAILED(device_->CreateTexture2D(&desc, nullptr, &texture_raw))) {
            initialization_failure_ = "stage_output_allocation_failed";
            release_outputs();
            return false;
          }
          textures_.emplace_back(texture_raw);
          ID3D11ShaderResourceView *srv_raw = nullptr;
          if (FAILED(device_->CreateShaderResourceView(texture_raw, nullptr, &srv_raw))) {
            initialization_failure_ = "stage_output_view_creation_failed";
            release_outputs();
            return false;
          }
          srvs_.emplace_back(srv_raw);

          output = foundation_stage_frame_t {
            .texture = texture_raw,
            .srv = srv_raw,
            .dxgi_format = desc.Format,
            .width = output_width_,
            .height = output_height_,
            .domain = caps->output_domain,
            .encoding = caps->output_encoding,
            .reference_white_nits = 80.0f,
            .frame_type = FOUNDATION_STAGE_FRAME_REAL,
            .source_generation = 0,
          };
        }

        foundation_stage_params_t params {
          .struct_size = sizeof(foundation_stage_params_t),
          .d3d11_device = device_,
          .d3d11_device_context = device_context_,
          .input_width = width,
          .input_height = height,
          .scale_hint = scale_hint_,
          .params_json = params_json_.empty() ? nullptr : params_json_.c_str(),
        };
        foundation_stage_status_e status;
        {
          std::lock_guard lock { dll_stage_mutex };
          status = loader_.stage_api()->create(&params, &instance_);
        }
        if (status != FOUNDATION_STAGE_STATUS_OK || !instance_) {
          initialization_failure_ = status == FOUNDATION_STAGE_STATUS_OK
                                      ? "stage_create_missing_instance"
                                      : stage_failure_reason(status, true);
          destroy_instance();
          release_outputs();
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
          std::lock_guard lock { dll_stage_mutex };
          loader_.stage_api()->destroy(instance_);
          instance_ = nullptr;
        }
        width_ = 0;
        height_ = 0;
      }

      void
      release_outputs() {
        outputs_.clear();
        for (auto *srv: srvs_) {
          if (srv) {
            srv->Release();
          }
        }
        srvs_.clear();
        for (auto *texture: textures_) {
          if (texture) {
            texture->Release();
          }
        }
        textures_.clear();
      }

      ID3D11Device *device_;
      ID3D11DeviceContext *device_context_;
      rtx_hdr::backend_loader_t loader_;
      float scale_hint_;
      std::string params_json_;
      void *instance_ = nullptr;
      std::string_view initialization_failure_ { "stage_initialization_failed" };
      std::vector<foundation_stage_frame_t> outputs_;
      std::vector<ID3D11Texture2D *> textures_;
      std::vector<ID3D11ShaderResourceView *> srvs_;
      std::uint32_t width_ = 0;
      std::uint32_t height_ = 0;
      std::uint32_t output_width_ = 0;
      std::uint32_t output_height_ = 0;
    };
  }  // namespace

  std::unique_ptr<pre_encode_filter_t>
  make_dll_stage_filter(
    rtx_hdr::backend_loader_t loader,
    ID3D11Device *device,
    ID3D11DeviceContext *device_context,
    float scale_hint,
    const std::string &params_json) {
    if (!loader || !loader.stage_v2() || !device || !device_context) {
      return {};
    }
    const auto *caps = loader.stage_api()->caps();
    if (caps->temporal) {
      BOOST_LOG(warning) << "Post-process stage '" << (caps->name ? caps->name : "?")
                         << "' declares temporal output; multi-frame batches are not driven yet (docs Phase 3)";
      return {};
    }
    // The ABI pins non-temporal stages to exactly one output frame; anything
    // else would allocate unbounded output textures for no drivable purpose.
    if (caps->max_frames_out != 1) {
      BOOST_LOG(warning) << "Post-process stage '" << (caps->name ? caps->name : "?")
                         << "' is non-temporal but declares max_frames_out=" << caps->max_frames_out
                         << "; the ABI requires exactly 1";
      return {};
    }
    if (caps->resolution_behavior == FOUNDATION_STAGE_RESOLUTION_ARBITRARY) {
      BOOST_LOG(warning) << "Post-process stage '" << (caps->name ? caps->name : "?")
                         << "' declares arbitrary resolution; not drivable yet";
      return {};
    }
    if (caps->resolution_behavior == FOUNDATION_STAGE_RESOLUTION_SCALE &&
        !(caps->min_scale <= scale_hint && scale_hint <= caps->max_scale)) {
      BOOST_LOG(warning) << "Post-process stage '" << (caps->name ? caps->name : "?")
                         << "' declares scale [" << caps->min_scale << ", " << caps->max_scale
                         << "] which does not include the requested " << scale_hint;
      return {};
    }
    return std::make_unique<dll_stage_filter_t>(
      device,
      device_context,
      std::move(loader),
      scale_hint,
      params_json);
  }
}  // namespace platf::dxgi::postprocess
