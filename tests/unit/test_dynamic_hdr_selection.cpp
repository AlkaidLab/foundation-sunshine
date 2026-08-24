/**
 * @file tests/unit/test_dynamic_hdr_selection.cpp
 * @brief Tests for the one-shot dynamic HDR negotiation.
 *
 * The matrix below is docs/dolby_vision_profile81.md §4.2: every gate that
 * can refuse Dolby Vision, the priority chain it falls through, and the
 * legacy-client compatibility rule that keeps unconditional HDR10+ for
 * clients that report no capabilities.
 */
#include <src/hdr/dynamic_hdr_selection.h>

#include <string_view>
#include <vector>

#include "../tests_common.h"

namespace {

  using hdr::dynamic_hdr_caps_e;
  using hdr::dynamic_hdr_format_e;
  using hdr::dynamic_hdr_fallback_e;
  using hdr::dynamic_hdr_host_gates_t;
  using hdr::dynamic_hdr_preference_e;
  using hdr::dynamic_hdr_request_t;
  using hdr::parse_dynamic_hdr_request;
  using hdr::select_dynamic_hdr;

  using hdr::DYNAMIC_HDR_CAPS_DOLBY_VISION_81;
  using hdr::DYNAMIC_HDR_CAPS_HDR10_PLUS;
  using hdr::DYNAMIC_HDR_CAPS_NONE;

  // A client that can do everything: HDR10+ + DV 8.1 with a direct surface.
  dynamic_hdr_request_t
  full_dv_client(dynamic_hdr_preference_e preference = dynamic_hdr_preference_e::automatic) {
    dynamic_hdr_request_t request;
    request.caps_mask = DYNAMIC_HDR_CAPS_HDR10_PLUS | DYNAMIC_HDR_CAPS_DOLBY_VISION_81;
    request.caps_reported = true;
    request.dolby_vision_direct_surface = true;
    request.dolby_vision_max_level = 30;  // ~4K60 in Dolby level terms
    request.preference = preference;
    return request;
  }

  dynamic_hdr_host_gates_t
  hevc_pq_host(bool dolby_vision_enabled = true) {
    return { .dolby_vision_enabled = dolby_vision_enabled, .video_format = 1, .dynamic_range_mode = 1 };
  }

}  // namespace

TEST(DynamicHdrSelection, SelectsDolbyVisionWhenEveryGatePasses) {
  const auto selection = select_dynamic_hdr(full_dv_client(), hevc_pq_host());
  EXPECT_EQ(selection.format, dynamic_hdr_format_e::dolby_vision_profile_81);
  EXPECT_TRUE(selection.dolby_vision_active());
  EXPECT_EQ(selection.fallback_reason, dynamic_hdr_fallback_e::none);
}

TEST(DynamicHdrSelection, ReportsEachRefusalReason) {
  struct case_t {
    dynamic_hdr_fallback_e reason;
    dynamic_hdr_format_e expected_format;
    dynamic_hdr_request_t request;
    dynamic_hdr_host_gates_t gates;
  };

  const auto with_caps = [](std::uint32_t mask, bool surface) {
    dynamic_hdr_request_t request;
    request.caps_mask = mask;
    request.caps_reported = true;
    request.dolby_vision_direct_surface = surface;
    return request;
  };

  const std::vector<case_t> cases {
    // Host switch off.
    { dynamic_hdr_fallback_e::host_disabled, dynamic_hdr_format_e::hdr10_plus,
      full_dv_client(), hevc_pq_host(false) },
    // DV 8.1 is HEVC-only. HDR10+ still has an AV1 carriage, so only H.264
    // loses both.
    { dynamic_hdr_fallback_e::codec_unsupported, dynamic_hdr_format_e::hdr10_plus,
      full_dv_client(),
      { .dolby_vision_enabled = true, .video_format = 2, .dynamic_range_mode = 1 } },
    { dynamic_hdr_fallback_e::codec_unsupported, dynamic_hdr_format_e::none,
      full_dv_client(),
      { .dolby_vision_enabled = true, .video_format = 0, .dynamic_range_mode = 1 } },
    // DV 8.1 rides an HDR10-compatible PQ base layer. Without PQ there is no
    // HDR10+ to fall through to either.
    { dynamic_hdr_fallback_e::colorspace_unsupported, dynamic_hdr_format_e::none,
      full_dv_client(),
      { .dolby_vision_enabled = true, .video_format = 1, .dynamic_range_mode = 0 } },
    { dynamic_hdr_fallback_e::colorspace_unsupported, dynamic_hdr_format_e::none,
      full_dv_client(),
      { .dolby_vision_enabled = true, .video_format = 1, .dynamic_range_mode = 2 } },
    // Client reported caps without the DV bit while explicitly asking for
    // Dolby Vision.
    { dynamic_hdr_fallback_e::client_caps_missing, dynamic_hdr_format_e::hdr10_plus,
      [&] {
        auto request = with_caps(DYNAMIC_HDR_CAPS_HDR10_PLUS, true);
        request.preference = dynamic_hdr_preference_e::dolby_vision;
        return request;
      }(),
      hevc_pq_host() },
    // DV must reach the display's Dolby engine untouched (docs §2.3).
    { dynamic_hdr_fallback_e::direct_surface_missing, dynamic_hdr_format_e::hdr10_plus,
      with_caps(DYNAMIC_HDR_CAPS_HDR10_PLUS | DYNAMIC_HDR_CAPS_DOLBY_VISION_81, false),
      hevc_pq_host() },
  };

  for (const auto &entry : cases) {
    const auto selection = select_dynamic_hdr(entry.request, entry.gates);
    EXPECT_EQ(selection.fallback_reason, entry.reason);
    EXPECT_EQ(selection.format, entry.expected_format);
    EXPECT_FALSE(selection.dolby_vision_active());
  }
}

TEST(DynamicHdrSelection, LegacyClientKeepsUnconditionalHdr10Plus) {
  // No capability report at all: the client keeps what every previous
  // Sunshine version sent, including with the DV switch off.
  dynamic_hdr_request_t legacy;
  const auto selection = select_dynamic_hdr(legacy, hevc_pq_host(false));
  EXPECT_EQ(selection.format, dynamic_hdr_format_e::hdr10_plus);
  EXPECT_EQ(selection.fallback_reason, dynamic_hdr_fallback_e::none);
}

TEST(DynamicHdrSelection, ReportedClientGetsNegotiatedDowngrade) {
  // A client that explicitly reported no HDR10+ bit is obeyed: plain HDR10.
  dynamic_hdr_request_t request;
  request.caps_mask = DYNAMIC_HDR_CAPS_NONE;
  request.caps_reported = true;

  const auto selection = select_dynamic_hdr(request, hevc_pq_host());
  EXPECT_EQ(selection.format, dynamic_hdr_format_e::none);
  EXPECT_EQ(selection.fallback_reason, dynamic_hdr_fallback_e::none);
}

TEST(DynamicHdrSelection, PreferenceDrivesThePriorityChain) {
  // Explicit Dolby Vision with full gates: DV wins.
  const auto dv = select_dynamic_hdr(full_dv_client(dynamic_hdr_preference_e::dolby_vision), hevc_pq_host());
  EXPECT_EQ(dv.format, dynamic_hdr_format_e::dolby_vision_profile_81);

  // Explicit HDR10+ from a DV-capable client: HDR10+ wins, preference is the
  // reported reason.
  const auto hdr10p = select_dynamic_hdr(full_dv_client(dynamic_hdr_preference_e::hdr10_plus), hevc_pq_host());
  EXPECT_EQ(hdr10p.format, dynamic_hdr_format_e::hdr10_plus);
  EXPECT_EQ(hdr10p.fallback_reason, dynamic_hdr_fallback_e::preference);

  // Explicit HDR10-only: no dynamic metadata at all.
  const auto hdr10 = select_dynamic_hdr(full_dv_client(dynamic_hdr_preference_e::hdr10_only), hevc_pq_host());
  EXPECT_EQ(hdr10.format, dynamic_hdr_format_e::none);
  EXPECT_EQ(hdr10.fallback_reason, dynamic_hdr_fallback_e::preference);
}

TEST(DynamicHdrSelection, AutomaticPreferenceLosesSilently) {
  // Automatic preference with an unavailable DV reports no fallback: the
  // client asked for nothing specific and needs no diagnostic.
  dynamic_hdr_request_t request;
  request.caps_mask = DYNAMIC_HDR_CAPS_HDR10_PLUS;
  request.caps_reported = true;

  const auto selection = select_dynamic_hdr(request, hevc_pq_host());
  EXPECT_EQ(selection.format, dynamic_hdr_format_e::hdr10_plus);
  EXPECT_EQ(selection.fallback_reason, dynamic_hdr_fallback_e::none);
}

TEST(DynamicHdrSelection, ParsesSdpArguments) {
  const auto request = parse_dynamic_hdr_request(
    std::string_view("9"),       // HDR10+ | DV 8.1
    std::string_view("30"),      // max level
    std::string_view("1"),       // direct surface
    std::string_view("1")        // preference: dolby_vision
  );
  EXPECT_TRUE(request.caps_reported);
  EXPECT_EQ(request.caps_mask,
    DYNAMIC_HDR_CAPS_HDR10_PLUS | DYNAMIC_HDR_CAPS_DOLBY_VISION_81);
  EXPECT_EQ(request.dolby_vision_max_level, 30u);
  EXPECT_TRUE(request.dolby_vision_direct_surface);
  EXPECT_EQ(request.preference, dynamic_hdr_preference_e::dolby_vision);
}

TEST(DynamicHdrSelection, MalformedArgumentsFallBackToLegacy) {
  // One bad field must not flip a legacy client into a negotiated downgrade.
  const auto garbage = parse_dynamic_hdr_request(
    std::string_view("not-a-number"), {}, {}, {});
  EXPECT_FALSE(garbage.caps_reported);
  EXPECT_EQ(garbage.caps_mask, 0u);
  EXPECT_EQ(garbage.preference, dynamic_hdr_preference_e::automatic);

  // Unknown capability bits reject the whole report rather than partially
  // trusting it.
  const auto unknown_bits = parse_dynamic_hdr_request(
    std::string_view("1000"), {}, {}, {});
  EXPECT_FALSE(unknown_bits.caps_reported);

  // Missing everything: the full legacy default.
  const auto empty = parse_dynamic_hdr_request({}, {}, {}, {});
  EXPECT_FALSE(empty.caps_reported);
  EXPECT_EQ(empty.preference, dynamic_hdr_preference_e::automatic);

  // Out-of-range level clamps to absent (0) instead of wrapping.
  const auto bad_level = parse_dynamic_hdr_request(
    std::string_view("9"), std::string_view("999999"), std::string_view("2"), std::string_view("7"));
  EXPECT_TRUE(bad_level.caps_reported);
  EXPECT_EQ(bad_level.dolby_vision_max_level, 0u);
  EXPECT_TRUE(bad_level.dolby_vision_direct_surface);
  // Unknown preference value falls back to automatic.
  EXPECT_EQ(bad_level.preference, dynamic_hdr_preference_e::automatic);
}

TEST(DynamicHdrSelection, WireValuesAreStable) {
  // These ride the RTSP response header; renumbering breaks clients.
  EXPECT_EQ(hdr::to_wire(dynamic_hdr_format_e::none), 0);
  EXPECT_EQ(hdr::to_wire(dynamic_hdr_format_e::hdr10_plus), 1);
  EXPECT_EQ(hdr::to_wire(dynamic_hdr_format_e::vivid_pq), 2);
  EXPECT_EQ(hdr::to_wire(dynamic_hdr_format_e::vivid_hlg), 3);
  EXPECT_EQ(hdr::to_wire(dynamic_hdr_format_e::dolby_vision_profile_81), 4);

  EXPECT_EQ(hdr::to_string(dynamic_hdr_format_e::dolby_vision_profile_81), "dolby_vision_profile_81");
  EXPECT_EQ(hdr::to_string(dynamic_hdr_fallback_e::direct_surface_missing), "direct_surface_missing");
}
