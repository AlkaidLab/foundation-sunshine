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
  using hdr::DYNAMIC_HDR_CAPS_DOLBY_VISION_84;
  using hdr::DYNAMIC_HDR_CAPS_HDR10_PLUS;
  using hdr::DYNAMIC_HDR_CAPS_NONE;

  // A client that can do everything: HDR10+ + DV 8.1 with a direct surface.
  dynamic_hdr_request_t
  full_dv_client(dynamic_hdr_preference_e preference = dynamic_hdr_preference_e::automatic) {
    dynamic_hdr_request_t request;
    request.caps_mask = DYNAMIC_HDR_CAPS_HDR10_PLUS | DYNAMIC_HDR_CAPS_DOLBY_VISION_81;
    request.caps_reported = true;
    request.dolby_vision_direct_surface = true;
    request.preference = preference;
    return request;
  }

  dynamic_hdr_host_gates_t
  hevc_pq_host() {
    return { .video_format = 1, .dynamic_range_mode = 1 };
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
    // DV 8.1 is HEVC-only. HDR10+ still has an AV1 carriage, so only H.264
    // loses both.
    { dynamic_hdr_fallback_e::codec_unsupported, dynamic_hdr_format_e::hdr10_plus,
      full_dv_client(),
      { .video_format = 2, .dynamic_range_mode = 1 } },
    { dynamic_hdr_fallback_e::codec_unsupported, dynamic_hdr_format_e::none,
      full_dv_client(),
      { .video_format = 0, .dynamic_range_mode = 1 } },
    // DV 8.1 rides an HDR10-compatible PQ base layer. Without PQ there is no
    // HDR10+ to fall through to either.
    { dynamic_hdr_fallback_e::colorspace_unsupported, dynamic_hdr_format_e::none,
      full_dv_client(),
      { .video_format = 1, .dynamic_range_mode = 0 } },
    { dynamic_hdr_fallback_e::colorspace_unsupported, dynamic_hdr_format_e::none,
      full_dv_client(),
      { .video_format = 1, .dynamic_range_mode = 2 } },
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
  // Sunshine version sent.
  dynamic_hdr_request_t legacy;
  const auto selection = select_dynamic_hdr(legacy, hevc_pq_host());
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
    std::string_view("1"),       // direct surface
    std::string_view("1")        // preference: dolby_vision
  );
  EXPECT_TRUE(request.caps_reported);
  EXPECT_EQ(request.caps_mask,
    DYNAMIC_HDR_CAPS_HDR10_PLUS | DYNAMIC_HDR_CAPS_DOLBY_VISION_81);
  EXPECT_TRUE(request.dolby_vision_direct_surface);
  EXPECT_EQ(request.preference, dynamic_hdr_preference_e::dolby_vision);

  // Unknown capability bits are masked off, not fatal: a future client keeps
  // its negotiation with the subset this host understands.
  const auto future = parse_dynamic_hdr_request(
    std::string_view("49"),      // HDR10+ | DV 8.4 | unknown bit 5
    std::string_view("1"),
    std::string_view("0"));
  EXPECT_TRUE(future.caps_reported);
  EXPECT_EQ(future.caps_mask,
    DYNAMIC_HDR_CAPS_HDR10_PLUS | DYNAMIC_HDR_CAPS_DOLBY_VISION_84);

  // A report carrying ONLY unknown bits is still a report: it must land in
  // the negotiated "no formats" downgrade, never back in the legacy path
  // that keeps unconditional HDR10+.
  const auto only_unknown = parse_dynamic_hdr_request(
    std::string_view("32"),      // unknown bit 5 alone
    std::string_view("0"),
    std::string_view("0"));
  EXPECT_TRUE(only_unknown.caps_reported);
  EXPECT_EQ(only_unknown.caps_mask, 0u);
  const auto only_unknown_selection = select_dynamic_hdr(only_unknown, hevc_pq_host());
  EXPECT_EQ(only_unknown_selection.format, dynamic_hdr_format_e::none);
}

TEST(DynamicHdrSelection, MalformedArgumentsFallBackToLegacy) {
  // One bad field must not flip a legacy client into a negotiated downgrade.
  const auto garbage = parse_dynamic_hdr_request(
    std::string_view("not-a-number"), {}, {});
  EXPECT_FALSE(garbage.caps_reported);
  EXPECT_EQ(garbage.caps_mask, 0u);
  EXPECT_EQ(garbage.preference, dynamic_hdr_preference_e::automatic);

  // An explicitly sent "0" is a real report of "no dynamic formats": only
  // the ANNOUNCE layer decides presence, so absent and explicit-zero must
  // stay distinguishable (the legacy-client downgrade regression).
  const auto explicit_zero = parse_dynamic_hdr_request(
    std::string_view("0"), {}, {});
  EXPECT_TRUE(explicit_zero.caps_reported);
  EXPECT_EQ(explicit_zero.caps_mask, 0u);

  // Unknown preference value falls back to automatic; a surface value is
  // parsed independently of the caps report.
  const auto bad_preference = parse_dynamic_hdr_request(
    std::string_view("9"), std::string_view("2"), std::string_view("7"));
  EXPECT_TRUE(bad_preference.caps_reported);
  EXPECT_TRUE(bad_preference.dolby_vision_direct_surface);
  EXPECT_EQ(bad_preference.preference, dynamic_hdr_preference_e::automatic);

  // Missing everything: the full legacy default.
  const auto empty = parse_dynamic_hdr_request({}, {}, {});
  EXPECT_FALSE(empty.caps_reported);
  EXPECT_EQ(empty.preference, dynamic_hdr_preference_e::automatic);
}

TEST(DynamicHdrSelection, WireValuesAreStable) {
  // These ride the RTSP response header; renumbering breaks clients.
  EXPECT_EQ(hdr::to_wire(dynamic_hdr_format_e::none), 0);
  EXPECT_EQ(hdr::to_wire(dynamic_hdr_format_e::hdr10_plus), 1);
  EXPECT_EQ(hdr::to_wire(dynamic_hdr_format_e::vivid_pq), 2);
  EXPECT_EQ(hdr::to_wire(dynamic_hdr_format_e::vivid_hlg), 3);
  EXPECT_EQ(hdr::to_wire(dynamic_hdr_format_e::dolby_vision_profile_81), 4);
  EXPECT_EQ(hdr::to_wire(dynamic_hdr_format_e::dolby_vision_profile_84), 5);

  EXPECT_EQ(hdr::to_string(dynamic_hdr_format_e::dolby_vision_profile_81), "dolby_vision_profile_81");
  EXPECT_EQ(hdr::to_string(dynamic_hdr_format_e::dolby_vision_profile_84), "dolby_vision_profile_84");
  EXPECT_EQ(hdr::to_string(dynamic_hdr_fallback_e::direct_surface_missing), "direct_surface_missing");
}

namespace {

  // An 8.4-only client: no 8.1 report, direct surface.
  dynamic_hdr_request_t
  dv84_client(dynamic_hdr_preference_e preference = dynamic_hdr_preference_e::automatic) {
    dynamic_hdr_request_t request;
    request.caps_mask = DYNAMIC_HDR_CAPS_HDR10_PLUS | DYNAMIC_HDR_CAPS_DOLBY_VISION_84;
    request.caps_reported = true;
    request.dolby_vision_direct_surface = true;
    request.preference = preference;
    return request;
  }

  dynamic_hdr_host_gates_t
  hevc_hlg_host() {
    return { .video_format = 1, .dynamic_range_mode = 2 };
  }

}  // namespace

TEST(DynamicHdrSelection, Selects84WhenOnly84ReportedAndHlgRequested) {
  const auto selection = select_dynamic_hdr(dv84_client(), hevc_hlg_host());
  EXPECT_EQ(selection.format, dynamic_hdr_format_e::dolby_vision_profile_84);
  EXPECT_TRUE(selection.dolby_vision_active());
  EXPECT_EQ(selection.fallback_reason, dynamic_hdr_fallback_e::none);
}

TEST(DynamicHdrSelection, AlwaysPrefers81WhenReported) {
  // Both bits reported + PQ: 8.1, never 8.4.
  dynamic_hdr_request_t both = dv84_client();
  both.caps_mask |= DYNAMIC_HDR_CAPS_DOLBY_VISION_81;
  const auto pq = select_dynamic_hdr(both, hevc_pq_host());
  EXPECT_EQ(pq.format, dynamic_hdr_format_e::dolby_vision_profile_81);

  // Both bits reported + HLG: 8.1 is unservable and 8.4 must not step in for
  // an 8.1-capable client (profile84.md §3.1) — colorspace refusal.
  const auto hlg = select_dynamic_hdr(both, hevc_hlg_host());
  EXPECT_EQ(hlg.format, dynamic_hdr_format_e::none);
  EXPECT_EQ(hlg.fallback_reason, dynamic_hdr_fallback_e::colorspace_unsupported);
}

TEST(DynamicHdrSelection, SyntheticHdrAppExcludes84ButNot81) {
  // An RTX HDR app is excluded from 8.4 for the whole app (profile84.md §2),
  // including the HLG ANNOUNCE path where the filter itself stays off —
  // the exclusion follows the app feature, not the per-session filter state.
  const auto hlg = select_dynamic_hdr(dv84_client(), { .video_format = 1, .dynamic_range_mode = 2, .synthetic_hdr_enabled = true });
  EXPECT_EQ(hlg.format, dynamic_hdr_format_e::none);
  EXPECT_EQ(hlg.fallback_reason, dynamic_hdr_fallback_e::colorspace_unsupported);

  // 8.1 rides the same PQ signal the filter produces, so it stays available.
  const auto pq = select_dynamic_hdr(full_dv_client(), { .video_format = 1, .dynamic_range_mode = 1, .synthetic_hdr_enabled = true });
  EXPECT_EQ(pq.format, dynamic_hdr_format_e::dolby_vision_profile_81);

  // Without the app feature the same HLG report still negotiates 8.4.
  const auto plain_hlg = select_dynamic_hdr(dv84_client(), hevc_hlg_host());
  EXPECT_EQ(plain_hlg.format, dynamic_hdr_format_e::dolby_vision_profile_84);
}

TEST(DynamicHdrSelection, EightyFourRefusalOnHlgHasNoHdr10PlusFallthrough) {
  // 8.4 needs a direct surface like 8.1 does; the refusal lands on plain
  // HDR10 because HDR10+ is PQ-only and this session is HLG.
  dynamic_hdr_request_t no_surface = dv84_client(dynamic_hdr_preference_e::dolby_vision);
  no_surface.dolby_vision_direct_surface = false;
  const auto selection = select_dynamic_hdr(no_surface, hevc_hlg_host());
  EXPECT_EQ(selection.fallback_reason, dynamic_hdr_fallback_e::direct_surface_missing);
  EXPECT_EQ(selection.format, dynamic_hdr_format_e::none);
  EXPECT_FALSE(selection.dolby_vision_active());
}
