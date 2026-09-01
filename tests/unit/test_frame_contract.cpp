#include <gtest/gtest.h>

#include "src/platform/frame_contract.h"
#ifdef _WIN32
  #include "src/platform/windows/frame_contract.h"
#endif

namespace {
  TEST(FrameContract, ResolvesSdrWithoutPrivateHandoff) {
    const auto policy = platf::resolve_frame_pipeline_policy(0, false);

    EXPECT_EQ(policy.output.transfer, platf::wire_transfer_e::sdr);
    EXPECT_FALSE(policy.output.require_10bit);
    EXPECT_EQ(policy.source_display, platf::source_display_intent_e::unchanged);
    EXPECT_EQ(policy.capture.required_domain, platf::frame_domain_e::sdr_rec709);
    EXPECT_EQ(policy.capture.preferred_encoding, platf::pixel_encoding_class_e::unorm8);
    EXPECT_FALSE(policy.capture.require_private_handoff);
  }

  TEST(FrameContract, ResolvesNativeHdrAsLinearFp16Capture) {
    const auto policy = platf::resolve_frame_pipeline_policy(1, false);

    EXPECT_EQ(policy.output.transfer, platf::wire_transfer_e::pq);
    EXPECT_TRUE(policy.output.require_10bit);
    EXPECT_TRUE(policy.output.require_bt2020);
    EXPECT_EQ(policy.source_display, platf::source_display_intent_e::require_hdr);
    EXPECT_EQ(policy.capture.required_domain, platf::frame_domain_e::linear_scrgb);
    EXPECT_EQ(policy.capture.preferred_encoding, platf::pixel_encoding_class_e::float16);
    EXPECT_FALSE(policy.capture.require_private_handoff);
  }

  TEST(FrameContract, PreservesHlgAndLegacyPositiveDynamicRangeSemantics) {
    const auto hlg = platf::resolve_frame_pipeline_policy(2, false);
    EXPECT_EQ(hlg.output.transfer, platf::wire_transfer_e::hlg);
    EXPECT_EQ(hlg.capture.preferred_encoding, platf::pixel_encoding_class_e::float16);

    const auto future_positive_value = platf::resolve_frame_pipeline_policy(3, false);
    EXPECT_EQ(future_positive_value.output.transfer, platf::wire_transfer_e::pq);
    EXPECT_EQ(future_positive_value.capture.preferred_encoding, platf::pixel_encoding_class_e::float16);
  }

  TEST(FrameContract, ResolvesPostProcessHdrAsPrivateSdrCapture) {
    const auto policy = platf::resolve_frame_pipeline_policy(1, true);

    EXPECT_EQ(policy.output.transfer, platf::wire_transfer_e::pq);
    EXPECT_EQ(policy.source_display, platf::source_display_intent_e::require_sdr);
    EXPECT_EQ(policy.capture.required_domain, platf::frame_domain_e::sdr_rec709);
    EXPECT_EQ(policy.capture.preferred_encoding, platf::pixel_encoding_class_e::unorm8);
    EXPECT_TRUE(policy.capture.require_private_handoff);
  }

  TEST(FrameContract, IgnoresPostProcessHdrForSdrWireOutput) {
    const auto policy = platf::resolve_frame_pipeline_policy(0, true);

    EXPECT_EQ(policy.output.transfer, platf::wire_transfer_e::sdr);
    EXPECT_EQ(policy.source_display, platf::source_display_intent_e::unchanged);
    EXPECT_FALSE(policy.capture.require_private_handoff);
  }

  TEST(FrameContract, PostprocessOutputOwnsHdrSignalIndependentlyOfCapture) {
    const auto post_process_hdr = platf::resolve_frame_pipeline_policy(1, true);
    EXPECT_EQ(post_process_hdr.capture.required_domain, platf::frame_domain_e::sdr_rec709);
    EXPECT_EQ(post_process_hdr.output.transfer, platf::wire_transfer_e::pq);
    EXPECT_TRUE(platf::postprocess_produces_hdr_output(
      post_process_hdr,
      platf::pre_encode_filter_e::external_sdr_to_hdr));

    const auto native_hdr = platf::resolve_frame_pipeline_policy(1, false);
    EXPECT_FALSE(platf::postprocess_produces_hdr_output(
      native_hdr,
      platf::pre_encode_filter_e::external_sdr_to_hdr));

    const auto disabled = platf::resolve_frame_pipeline_policy(0, false);
    EXPECT_FALSE(platf::postprocess_produces_hdr_output(
      disabled,
      platf::pre_encode_filter_e::none));
  }

  TEST(FrameContract, RejectsUnknownOrBorrowedFramesWhenPrivateInputIsRequired) {
    const platf::capture_contract_t required {
      .required_domain = platf::frame_domain_e::sdr_rec709,
      .preferred_encoding = platf::pixel_encoding_class_e::unorm8,
      .require_private_handoff = true,
    };

    platf::captured_frame_desc_t actual;
    EXPECT_FALSE(platf::frame_satisfies_capture_contract(required, actual));

    actual.domain = platf::frame_domain_e::sdr_rec709;
    actual.encoding = platf::pixel_encoding_class_e::unorm8;
    actual.borrowed = true;
    EXPECT_FALSE(platf::frame_satisfies_capture_contract(required, actual));

    actual.borrowed = false;
    EXPECT_TRUE(platf::frame_satisfies_capture_contract(required, actual));
  }

  TEST(FrameContract, RejectsDomainAndEncodingMismatch) {
    const platf::capture_contract_t required {
      .required_domain = platf::frame_domain_e::linear_scrgb,
      .preferred_encoding = platf::pixel_encoding_class_e::float16,
    };
    platf::captured_frame_desc_t actual {
      .domain = platf::frame_domain_e::sdr_rec709,
      .encoding = platf::pixel_encoding_class_e::unorm8,
    };

    EXPECT_FALSE(platf::frame_satisfies_capture_contract(required, actual));
  }

#ifdef _WIN32
  TEST(FrameContract, MapsWgcFormatFromCaptureContractOnly) {
    const auto sdr = platf::resolve_frame_pipeline_policy(0, false);
    const auto native_hdr = platf::resolve_frame_pipeline_policy(1, false);
    const auto post_process_hdr = platf::resolve_frame_pipeline_policy(1, true);

    EXPECT_EQ(
      platf::dxgi::select_wgc_capture_format(sdr.capture),
      DXGI_FORMAT_B8G8R8A8_UNORM);
    EXPECT_EQ(
      platf::dxgi::select_wgc_capture_format(native_hdr.capture),
      DXGI_FORMAT_R16G16B16A16_FLOAT);
    EXPECT_EQ(
      platf::dxgi::select_wgc_capture_format(post_process_hdr.capture),
      DXGI_FORMAT_B8G8R8A8_UNORM);
  }

  TEST(FrameContract, DescribesDxgiOwnershipAndColorSemantics) {
    const auto borrowed_sdr = platf::dxgi::describe_dxgi_captured_frame(
      DXGI_FORMAT_B8G8R8A8_UNORM, false, true, 42, 7);
    EXPECT_EQ(borrowed_sdr.domain, platf::frame_domain_e::sdr_rec709);
    EXPECT_EQ(borrowed_sdr.encoding, platf::pixel_encoding_class_e::unorm8);
    EXPECT_TRUE(borrowed_sdr.borrowed);
    EXPECT_EQ(borrowed_sdr.adapter_luid, 42u);
    EXPECT_EQ(borrowed_sdr.source_generation, 7u);

    const auto linear_hdr = platf::dxgi::describe_dxgi_captured_frame(
      DXGI_FORMAT_R16G16B16A16_FLOAT, true, false, 42, 8);
    EXPECT_EQ(linear_hdr.domain, platf::frame_domain_e::linear_scrgb);
    EXPECT_EQ(linear_hdr.encoding, platf::pixel_encoding_class_e::float16);
    EXPECT_FALSE(linear_hdr.borrowed);

    const auto unproven_fp16 = platf::dxgi::describe_dxgi_captured_frame(
      DXGI_FORMAT_R16G16B16A16_FLOAT, false, false, 42, 9);
    EXPECT_EQ(unproven_fp16.domain, platf::frame_domain_e::unknown);
  }
#endif
}  // namespace
