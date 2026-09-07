/**
 * @file tests/unit/platform/windows/test_postprocess_chain.cpp
 * @brief Tests for the post-process chain: pure validator rules plus the
 * WARP-driven chain executor and stage-ABI v2 driver.
 *
 * The validator half runs without a GPU; the driver half uses the same WARP
 * fixture as the pre-encode filter tests. Fake v2 DLLs are built alongside
 * (tests/tools/fake_stage_backend.cpp with per-variant defines).
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include <d3d11.h>

#include "src/platform/windows/postprocess/chain_validator.h"
#include "src/platform/windows/postprocess/stage_chain.h"
#include "src/platform/windows/pre_encode_filter.h"

namespace {
  using platf::dxgi::filter_status_e;
  using platf::dxgi::gpu_frame_view_t;
  using platf::dxgi::make_pre_encode_filter;
  using platf::dxgi::postprocess::chain_limits_t;
  using platf::dxgi::postprocess::make_dll_stage_filter;
  using platf::dxgi::postprocess::make_stage_chain;
  using platf::dxgi::postprocess::stage_declaration_t;
  using platf::dxgi::postprocess::synthesize_v1_declaration;

  constexpr auto kSdr = FOUNDATION_STAGE_DOMAIN_SDR_REC709;
  constexpr auto kScrgb = FOUNDATION_STAGE_DOMAIN_LINEAR_SCRGB;
  constexpr auto kUnorm8 = FOUNDATION_STAGE_ENCODING_UNORM8;
  constexpr auto kFloat16 = FOUNDATION_STAGE_ENCODING_FLOAT16;

  stage_declaration_t
  declare(std::string name, std::uint32_t in, std::uint32_t in_enc, std::uint32_t out, std::uint32_t out_enc) {
    stage_declaration_t declaration;
    declaration.name = std::move(name);
    declaration.input_domain = in;
    declaration.input_encoding = in_enc;
    declaration.output_domain = out;
    declaration.output_encoding = out_enc;
    return declaration;
  }

  // ------------------------------------------------------------------
  // Validator (pure)
  // ------------------------------------------------------------------
  TEST(PostprocessChainValidator, EmptyChainWithMatchingLegsIsOk) {
    const auto plan = platf::dxgi::postprocess::validate_chain({}, kSdr, kUnorm8, kSdr, kUnorm8);
    EXPECT_TRUE(plan.ok);
    EXPECT_TRUE(plan.entries.empty());
    EXPECT_FALSE(plan.temporal);
  }

  TEST(PostprocessChainValidator, V1MigrationChainNeedsNoConversion) {
    // The rtx_hdr migration: single v1 TrueHDR stage, capture BGRA8, HDR
    // encoder leg consuming FP16 scRGB. Every boundary meets directly.
    const std::vector<stage_declaration_t> stages { synthesize_v1_declaration("truehdr") };
    const auto plan = platf::dxgi::postprocess::validate_chain(stages, kSdr, kUnorm8, kScrgb, kFloat16);
    EXPECT_TRUE(plan.ok);
    ASSERT_EQ(plan.entries.size(), 1u);
    EXPECT_EQ(plan.entries[0].name, "truehdr");
    EXPECT_FALSE(plan.entries[0].builtin_conversion);
  }

  TEST(PostprocessChainValidator, InsertsBuiltinConversionAtDomainGap) {
    // An SDR-out stage feeding an scRGB-in stage: the linearize pass must be
    // auto-inserted between them.
    const std::vector<stage_declaration_t> stages {
      declare("pass_sdr", kSdr, kUnorm8, kSdr, kUnorm8),
      declare("hdr", kScrgb, kFloat16, kScrgb, kFloat16),
    };
    const auto plan = platf::dxgi::postprocess::validate_chain(stages, kSdr, kUnorm8, kScrgb, kFloat16);
    EXPECT_TRUE(plan.ok);
    ASSERT_EQ(plan.entries.size(), 3u);
    EXPECT_EQ(plan.entries[0].name, "pass_sdr");
    EXPECT_TRUE(plan.entries[1].builtin_conversion);
    EXPECT_EQ(plan.entries[1].name, "builtin_linearize");
    EXPECT_EQ(plan.entries[2].name, "hdr");
  }

  TEST(PostprocessChainValidator, UnreachableBoundaryIsRejected) {
    // scRGB -> SDR has no builtin edge: R2 rejection naming the gap.
    const std::vector<stage_declaration_t> stages {
      declare("hdr", kSdr, kUnorm8, kScrgb, kFloat16),
      declare("sdr_stage", kSdr, kUnorm8, kSdr, kUnorm8),
    };
    const auto plan = platf::dxgi::postprocess::validate_chain(stages, kSdr, kUnorm8, kSdr, kUnorm8);
    EXPECT_FALSE(plan.ok);
    ASSERT_FALSE(plan.errors.empty());
    EXPECT_NE(plan.errors[0].find("R2_conversion_unavailable"), std::string::npos);
    EXPECT_NE(plan.errors[0].find("linear_scrgb"), std::string::npos);
  }

  TEST(PostprocessChainValidator, UnsupportedAbiIsExcluded) {
    auto alien = declare("alien", kSdr, kUnorm8, kScrgb, kFloat16);
    alien.abi_version = 3;
    const std::vector<stage_declaration_t> stages { alien };
    const auto plan = platf::dxgi::postprocess::validate_chain(stages, kSdr, kUnorm8, kScrgb, kFloat16);
    EXPECT_FALSE(plan.ok);
    ASSERT_FALSE(plan.errors.empty());
    EXPECT_NE(plan.errors[0].find("R1_abi_unsupported"), std::string::npos);
  }

  TEST(PostprocessChainValidator, ScaleProductMustRespectEncoderBound) {
    chain_limits_t limits { .max_resolution_scale = 2.0f, .max_frame_multiplier = 2 };
    auto up = declare("up", kSdr, kUnorm8, kSdr, kUnorm8);
    up.resolution_behavior = FOUNDATION_STAGE_RESOLUTION_SCALE;
    up.min_scale = 1.0f;
    up.max_scale = 2.0f;

    const std::vector<stage_declaration_t> one { up };
    EXPECT_TRUE(platf::dxgi::postprocess::validate_chain(one, kSdr, kUnorm8, kSdr, kUnorm8, limits).ok);

    const std::vector<stage_declaration_t> two { up, up };
    const auto plan = platf::dxgi::postprocess::validate_chain(two, kSdr, kUnorm8, kSdr, kUnorm8, limits);
    EXPECT_FALSE(plan.ok);
    ASSERT_FALSE(plan.errors.empty());
    EXPECT_NE(plan.errors[0].find("R3_scale_exceeds_encoder"), std::string::npos);
  }

  TEST(PostprocessChainValidator, TemporalBoundsAndWarnings) {
    auto fg = declare("fg", kScrgb, kFloat16, kScrgb, kFloat16);
    fg.temporal = true;
    fg.max_frames_out = 2;

    // Single temporal x2 within bounds: accepted with the cost warning.
    const auto ok_plan = platf::dxgi::postprocess::validate_chain({ fg }, kSdr, kUnorm8, kScrgb, kFloat16);
    EXPECT_TRUE(ok_plan.ok);
    EXPECT_TRUE(ok_plan.temporal);
    EXPECT_EQ(ok_plan.frame_multiplier, 2u);
    ASSERT_FALSE(ok_plan.warnings.empty());
    EXPECT_NE(ok_plan.warnings[0].find("R5_temporal_cost"), std::string::npos);

    // Two temporal stages: multiplier x4 exceeds the encoder bound.
    const auto over_plan = platf::dxgi::postprocess::validate_chain({ fg, fg }, kSdr, kUnorm8, kScrgb, kFloat16);
    EXPECT_FALSE(over_plan.ok);
    bool found_r4 = false;
    for (const auto &error: over_plan.errors) {
      found_r4 |= error.find("R4_frame_multiplier_exceeds_encoder") != std::string::npos;
    }
    EXPECT_TRUE(found_r4);
    bool found_multi = false;
    for (const auto &warning: over_plan.warnings) {
      found_multi |= warning.find("R4_multiple_temporal") != std::string::npos;
    }
    EXPECT_TRUE(found_multi);
  }

  TEST(PostprocessChainValidator, SdrChainIntoHdrWireWarns) {
    // The HDR encoder leg consumes FP16 scRGB; a chain whose every stage
    // output stays SDR is SDR-in-HDR (R6), accepted with a warning.
    const std::vector<stage_declaration_t> stages { declare("sharpen", kSdr, kUnorm8, kSdr, kUnorm8) };
    const auto plan = platf::dxgi::postprocess::validate_chain(stages, kSdr, kUnorm8, kScrgb, kFloat16);
    EXPECT_TRUE(plan.ok);
    ASSERT_EQ(plan.entries.size(), 2u);  // sharpen + auto-inserted linearize
    EXPECT_TRUE(plan.entries[1].builtin_conversion);
    ASSERT_FALSE(plan.warnings.empty());
    EXPECT_NE(plan.warnings[0].find("R6_sdr_in_hdr_container"), std::string::npos);
  }

  // ------------------------------------------------------------------
  // Stage ABI v2 loader + driver + chain executor (WARP)
  // ------------------------------------------------------------------
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

  struct d3d_fixture_t {
    com_ptr_t<ID3D11Device> device;
    com_ptr_t<ID3D11DeviceContext> context;

    bool
    init() {
      ID3D11Device *device_raw = nullptr;
      ID3D11DeviceContext *context_raw = nullptr;
      const auto status = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_WARP,
        nullptr,
        0,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &device_raw,
        nullptr,
        &context_raw);
      if (FAILED(status)) {
        return false;
      }
      device.reset(device_raw);
      context.reset(context_raw);
      return true;
    }
  };

  struct input_texture_t {
    com_ptr_t<ID3D11Texture2D> texture;
    com_ptr_t<ID3D11ShaderResourceView> srv;
  };

  input_texture_t
  make_white_input(ID3D11Device *device, std::uint32_t width, std::uint32_t height) {
    std::vector<std::uint32_t> pixels(width * height, 0xFFFFFFFFu);
    D3D11_SUBRESOURCE_DATA initial_data {
      .pSysMem = pixels.data(),
      .SysMemPitch = static_cast<UINT>(width * sizeof(std::uint32_t)),
    };
    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    input_texture_t result;
    ID3D11Texture2D *texture_raw = nullptr;
    if (FAILED(device->CreateTexture2D(&desc, &initial_data, &texture_raw))) {
      return {};
    }
    result.texture.reset(texture_raw);
    ID3D11ShaderResourceView *srv_raw = nullptr;
    if (FAILED(device->CreateShaderResourceView(texture_raw, nullptr, &srv_raw))) {
      return {};
    }
    result.srv.reset(srv_raw);
    return result;
  }

  gpu_frame_view_t
  sdr_view(const input_texture_t &input, std::uint32_t width, std::uint32_t height, std::uint64_t generation) {
    return {
      .texture = input.texture.get(),
      .srv = input.srv.get(),
      .format = DXGI_FORMAT_B8G8R8A8_UNORM,
      .semantic = {
        .domain = platf::frame_domain_e::sdr_rec709,
        .encoding = platf::pixel_encoding_class_e::unorm8,
        .reference_white_nits = 80.0f,
        .borrowed = false,
        .source_generation = generation,
      },
      .width = width,
      .height = height,
    };
  }

  platf::dxgi::rtx_hdr::backend_loader_t
  load_fake_stage(const char *path) {
    platf::dxgi::rtx_hdr::backend_loader_t loader;
    if (!loader.load(std::filesystem::path(path))) {
      return {};
    }
    return loader;
  }

  TEST(PostprocessStageLoader, ProbesV2BeforeV1) {
    auto loader = load_fake_stage(FAKE_STAGE_BACKEND_PATH);
    ASSERT_TRUE(loader);
    EXPECT_TRUE(loader.stage_v2());
    ASSERT_NE(loader.stage_api(), nullptr);
    EXPECT_STREQ(loader.stage_api()->caps()->name, "fake.v2stage");
  }

  TEST(PostprocessStageLoader, RejectsV2AbiMismatch) {
    platf::dxgi::rtx_hdr::backend_loader_t loader;
    EXPECT_FALSE(loader.load(std::filesystem::path(FAKE_STAGE_BAD_BACKEND_PATH)));
    EXPECT_EQ(loader.error(), "stage_abi_mismatch");
  }

  TEST(PostprocessStageDriver, RunsV2StageThroughChainEngine) {
    d3d_fixture_t d3d;
    ASSERT_TRUE(d3d.init());

    auto filter = make_dll_stage_filter(
      load_fake_stage(FAKE_STAGE_BACKEND_PATH),
      d3d.device.get(),
      d3d.context.get(),
      1.0f,
      {});
    ASSERT_TRUE(filter);
    EXPECT_EQ(filter->backend_name(), "fake.v2stage");
    EXPECT_FALSE(filter->degraded());

    auto input = make_white_input(d3d.device.get(), 4, 4);
    ASSERT_TRUE(input.texture);
    const auto result = filter->process(sdr_view(input, 4, 4, 21));
    ASSERT_EQ(result.status, filter_status_e::ready);
    EXPECT_EQ(result.frame.format, DXGI_FORMAT_R16G16B16A16_FLOAT);
    EXPECT_EQ(result.frame.semantic.domain, platf::frame_domain_e::linear_scrgb);
    EXPECT_EQ(result.frame.semantic.encoding, platf::pixel_encoding_class_e::float16);
    EXPECT_FALSE(result.frame.semantic.borrowed);
    EXPECT_EQ(result.frame.semantic.source_generation, 21u);
  }

  TEST(PostprocessStageDriver, ProcessFailurePropagatesStageReason) {
    d3d_fixture_t d3d;
    ASSERT_TRUE(d3d.init());

    auto filter = make_dll_stage_filter(
      load_fake_stage(FAKE_STAGE_FAILING_BACKEND_PATH),
      d3d.device.get(),
      d3d.context.get(),
      1.0f,
      {});
    ASSERT_TRUE(filter);

    auto input = make_white_input(d3d.device.get(), 4, 4);
    const auto result = filter->process(sdr_view(input, 4, 4, 1));
    EXPECT_EQ(result.status, filter_status_e::failed);
    EXPECT_EQ(result.reason, "stage_process_internal_error");
  }

  TEST(PostprocessStageDriver, TemporalStageRefusedUntilPhase3) {
    d3d_fixture_t d3d;
    ASSERT_TRUE(d3d.init());

    auto loader = load_fake_stage(FAKE_STAGE_TEMPORAL_BACKEND_PATH);
    ASSERT_TRUE(loader);
    EXPECT_EQ(loader.stage_api()->caps()->max_frames_out, 2u);
    EXPECT_FALSE(make_dll_stage_filter(std::move(loader), d3d.device.get(), d3d.context.get(), 1.0f, {}));
  }

  TEST(PostprocessChain, BypassedStageKeepsRemainingChainAlive) {
    d3d_fixture_t d3d;
    ASSERT_TRUE(d3d.init());

    // First stage is a raw failing v2 DLL stage (no failover wrapper inside —
    // the rtx_hdr make() path always wraps, which would self-heal), second is
    // the mock SDR->scRGB shader: the failing stage is bypassed and the frame
    // still comes out as scRGB from the mock.
    auto failing = make_dll_stage_filter(
      load_fake_stage(FAKE_STAGE_FAILING_BACKEND_PATH),
      d3d.device.get(),
      d3d.context.get(),
      1.0f,
      {});
    auto mock = make_pre_encode_filter(
      platf::pre_encode_filter_e::mock_sdr_to_scrgb,
      d3d.device.get(),
      d3d.context.get());
    ASSERT_TRUE(failing);
    ASSERT_TRUE(mock);

    std::vector<std::unique_ptr<platf::dxgi::pre_encode_filter_t>> stages;
    stages.push_back(std::move(failing));
    stages.push_back(std::move(mock));
    auto chain = make_stage_chain(std::move(stages));
    ASSERT_TRUE(chain);
    EXPECT_EQ(chain->backend_name(), "postprocess_chain");

    auto input = make_white_input(d3d.device.get(), 4, 4);
    const auto result = chain->process(sdr_view(input, 4, 4, 30));
    ASSERT_EQ(result.status, filter_status_e::ready);
    EXPECT_EQ(result.frame.semantic.domain, platf::frame_domain_e::linear_scrgb);
    EXPECT_EQ(result.frame.semantic.source_generation, 30u);
    EXPECT_TRUE(chain->degraded());
    EXPECT_EQ(chain->failure_reason(), "stage_process_internal_error");

    // Per-slot reporting for /api/runtime/postprocess: the bypassed slot
    // keeps its name and reason, the surviving slot stays active.
    const auto *chain_impl = dynamic_cast<platf::dxgi::postprocess::chain_filter_t *>(chain.get());
    ASSERT_NE(chain_impl, nullptr);
    const auto states = chain_impl->stage_states();
    ASSERT_EQ(states.size(), 2u);
    EXPECT_EQ(states[0].state, "bypassed");
    EXPECT_EQ(states[0].failure_reason, "stage_process_internal_error");
    EXPECT_EQ(states[1].state, "active");
    EXPECT_TRUE(states[1].failure_reason.empty());
  }

  TEST(PostprocessChain, EmptyChainAfterBypassFailsWithChildReason) {
    d3d_fixture_t d3d;
    ASSERT_TRUE(d3d.init());

    auto failing = make_dll_stage_filter(
      load_fake_stage(FAKE_STAGE_FAILING_BACKEND_PATH),
      d3d.device.get(),
      d3d.context.get(),
      1.0f,
      {});
    ASSERT_TRUE(failing);
    std::vector<std::unique_ptr<platf::dxgi::pre_encode_filter_t>> stages;
    stages.push_back(std::move(failing));
    auto chain = make_stage_chain(std::move(stages));
    ASSERT_TRUE(chain);

    auto input = make_white_input(d3d.device.get(), 4, 4);
    const auto result = chain->process(sdr_view(input, 4, 4, 1));
    EXPECT_EQ(result.status, filter_status_e::failed);
    EXPECT_EQ(result.reason, "stage_process_internal_error");
  }

  TEST(PostprocessChain, SingleStageChainIsTransparent) {
    d3d_fixture_t d3d;
    ASSERT_TRUE(d3d.init());

    // The rtx_hdr migration shape: a one-element chain reports the child's
    // backend name, and the existing pre-encode filter tests keep pinning
    // the failover behavior around it.
    auto external = make_pre_encode_filter(
      platf::pre_encode_filter_e::external_sdr_to_hdr,
      d3d.device.get(),
      d3d.context.get(),
      std::filesystem::path(FAKE_TRUEHDR_BACKEND_PATH));
    ASSERT_TRUE(external);
    std::vector<std::unique_ptr<platf::dxgi::pre_encode_filter_t>> stages;
    stages.push_back(std::move(external));
    auto chain = make_stage_chain(std::move(stages));
    ASSERT_TRUE(chain);
    EXPECT_EQ(chain->backend_name(), "external_sdr_to_hdr");
  }
}  // namespace
