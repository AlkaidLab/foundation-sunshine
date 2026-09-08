#include <gtest/gtest.h>

#include "src/hdr/client_display_capabilities.h"

namespace {
  using hdr::parse_client_display_capabilities;
  using hdr::resolve_effective_target;
  using hdr::target_source_e;
  using hdr::validate_client_display_capabilities;

  TEST(ClientHdrCapabilities, UsesDefaultsWhenClientDoesNotReport) {
    const auto result = parse_client_display_capabilities(std::nullopt, std::nullopt, std::nullopt);

    EXPECT_FALSE(result.capabilities.reported);
    EXPECT_TRUE(result.fallback_reason.empty());
    EXPECT_FLOAT_EQ(result.capabilities.max_nits, 1000.0f);
    EXPECT_FLOAT_EQ(result.capabilities.min_nits, 0.001f);
    EXPECT_FLOAT_EQ(result.capabilities.max_full_frame_nits, 1000.0f);
  }

  TEST(ClientHdrCapabilities, AcceptsACompleteValidReport) {
    const auto result = parse_client_display_capabilities("1200", "0.005", "600");

    EXPECT_TRUE(result.capabilities.reported);
    EXPECT_TRUE(result.fallback_reason.empty());
    EXPECT_FLOAT_EQ(result.capabilities.max_nits, 1200.0f);
    EXPECT_FLOAT_EQ(result.capabilities.min_nits, 0.005f);
    EXPECT_FLOAT_EQ(result.capabilities.max_full_frame_nits, 600.0f);
  }

  TEST(ClientHdrCapabilities, RejectsPartialReportsAtomically) {
    const auto result = parse_client_display_capabilities("1200", std::nullopt, "600");

    EXPECT_FALSE(result.capabilities.reported);
    EXPECT_FALSE(result.fallback_reason.empty());
    EXPECT_FLOAT_EQ(result.capabilities.max_nits, 1000.0f);
  }

  TEST(ClientHdrCapabilities, RejectsMalformedAndNonFiniteValues) {
    EXPECT_FALSE(parse_client_display_capabilities("bright", "0.001", "500").capabilities.reported);
    EXPECT_FALSE(parse_client_display_capabilities("nan", "0.001", "500").capabilities.reported);
    EXPECT_FALSE(parse_client_display_capabilities("inf", "0.001", "500").capabilities.reported);
  }

  TEST(ClientHdrCapabilities, RejectsInvalidRangesAndRelationships) {
    EXPECT_FALSE(parse_client_display_capabilities("10001", "0.001", "500").capabilities.reported);
    EXPECT_FALSE(parse_client_display_capabilities("1000", "-0.1", "500").capabilities.reported);
    EXPECT_FALSE(parse_client_display_capabilities("1000", "0.001", "1200").capabilities.reported);
    EXPECT_FALSE(parse_client_display_capabilities("1000", "600", "500").capabilities.reported);
  }

  TEST(ClientHdrCapabilities, ValidatesParsedFloatsWithoutRounding) {
    const auto precise = validate_client_display_capabilities(1000.0f, 0.0000004f, 999.99994f);
    ASSERT_TRUE(precise.capabilities.reported);
    EXPECT_FLOAT_EQ(precise.capabilities.min_nits, 0.0000004f);
    EXPECT_FLOAT_EQ(precise.capabilities.max_full_frame_nits, 999.99994f);

    EXPECT_FALSE(validate_client_display_capabilities(0.99999994f, 0.0f, 1.0f).capabilities.reported);
  }

  TEST(ClientHdrCapabilities, UsesClientReportInAutomaticMode) {
    const auto reported = parse_client_display_capabilities("1200", "0.005", "600").capabilities;
    const auto target = resolve_effective_target(
      R"([{"uuid":"client-1","hdrBrightnessMode":"auto"}])", "client-1", "Client", reported);

    ASSERT_TRUE(target);
    EXPECT_EQ(target.source, target_source_e::client_report);
    EXPECT_FLOAT_EQ(target.capabilities.max_nits, 1200.0f);
  }

  TEST(ClientHdrCapabilities, AppliesManualOverrideByUuid) {
    const auto reported = parse_client_display_capabilities("1200", "0.005", "600").capabilities;
    const auto target = resolve_effective_target(
      R"([{"uuid":"client-1","hdrBrightnessMode":"manual","hdrBrightnessMaxNits":800,"hdrBrightnessMinNits":0.01,"hdrBrightnessMaxFullFrameNits":400}])",
      "client-1", "Client", reported);

    ASSERT_TRUE(target);
    EXPECT_EQ(target.source, target_source_e::manual_override);
    EXPECT_FLOAT_EQ(target.capabilities.max_nits, 800.0f);
    EXPECT_FLOAT_EQ(target.capabilities.min_nits, 0.01f);
    EXPECT_FLOAT_EQ(target.capabilities.max_full_frame_nits, 400.0f);
  }

  TEST(ClientHdrCapabilities, DoesNotUseNameWhenUuidIsAvailable) {
    const auto target = resolve_effective_target(
      R"([{"uuid":"other","name":"Client","hdrBrightnessMode":"manual","hdrBrightnessMaxNits":800,"hdrBrightnessMinNits":0.01,"hdrBrightnessMaxFullFrameNits":400}])",
      "client-1", "Client", {});

    ASSERT_TRUE(target);
    EXPECT_EQ(target.source, target_source_e::safe_defaults);
    EXPECT_FLOAT_EQ(target.capabilities.max_nits, 1000.0f);
  }

  TEST(ClientHdrCapabilities, RejectsInvalidManualOverride) {
    const auto target = resolve_effective_target(
      R"([{"uuid":"client-1","hdrBrightnessMode":"manual","hdrBrightnessMaxNits":400,"hdrBrightnessMinNits":0.01,"hdrBrightnessMaxFullFrameNits":800}])",
      "client-1", "Client", {});

    EXPECT_FALSE(target);
    EXPECT_EQ(target.source, target_source_e::safe_defaults);
  }

  TEST(ClientHdrCapabilities, NamesWindowsCalibrationSource) {
    EXPECT_EQ(hdr::to_string(target_source_e::windows_hdr_calibration), "windows_hdr_calibration");
  }
}  // namespace
