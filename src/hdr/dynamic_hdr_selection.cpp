/**
 * @file src/hdr/dynamic_hdr_selection.cpp
 * @brief Dynamic HDR format negotiation for a streaming session.
 */
#include "dynamic_hdr_selection.h"

#include <charconv>
#include <string_view>

namespace hdr {

  namespace {

    std::optional<std::uint64_t>
    parse_uint(std::string_view text) noexcept {
      std::uint64_t value = 0;
      const char *begin = text.data();
      const char *end = begin + text.size();
      const auto [position, error] = std::from_chars(begin, end, value);
      if (error != std::errc {} || position != end) {
        return std::nullopt;
      }
      return value;
    }

  }  // namespace

  dynamic_hdr_selection_t
  select_dynamic_hdr(
    const dynamic_hdr_request_t &request,
    const dynamic_hdr_host_gates_t &gates) noexcept {
    const bool dv_preferred = request.preference == dynamic_hdr_preference_e::automatic ||
                              request.preference == dynamic_hdr_preference_e::dolby_vision;
    const bool caps_81 = (request.caps_mask & DYNAMIC_HDR_CAPS_DOLBY_VISION_81) != 0;
    const bool caps_84 = (request.caps_mask & DYNAMIC_HDR_CAPS_DOLBY_VISION_84) != 0;
    // "Requested" covers both a capability the client offered and an explicit
    // preference for Dolby Vision — the latter without the former is a client
    // bug worth a diagnostic, not silence.
    const bool dv_requested = dv_preferred &&
                              ((caps_81 || caps_84) ||
                                request.preference == dynamic_hdr_preference_e::dolby_vision);

    // The DV verdict first: a selection returns immediately, a refusal falls
    // through to the HDR10+ chain while keeping the reason it lost.
    if (dv_preferred) {
      std::optional<dynamic_hdr_fallback_e> dv_fallback;
      if (gates.video_format != 1) {
        dv_fallback = dynamic_hdr_fallback_e::codec_unsupported;
      }
      else if (!caps_81 && !caps_84) {
        dv_fallback = dynamic_hdr_fallback_e::client_caps_missing;
      }
      else if (caps_81 && gates.dynamic_range_mode == 1) {
        // 8.1 wins whenever it is servable, regardless of an 8.4 report
        // (profile84.md §3.1: PQ base layer is the priority channel).
        if (!request.dolby_vision_direct_surface) {
          dv_fallback = dynamic_hdr_fallback_e::direct_surface_missing;
        }
        else {
          return { dynamic_hdr_format_e::dolby_vision_profile_81, dynamic_hdr_fallback_e::none };
        }
      }
      else if (caps_84 && !caps_81 && gates.dynamic_range_mode == 2) {
        // 8.4 is the HLG-base-layer channel. An app with the SDR-to-HDR
        // feature (RTX HDR) is excluded from it outright (profile84.md §2),
        // and so is an 8.1 report, which keeps PQ the expected transfer.
        if (gates.synthetic_hdr_enabled) {
          dv_fallback = dynamic_hdr_fallback_e::colorspace_unsupported;
        }
        else if (!request.dolby_vision_direct_surface) {
          dv_fallback = dynamic_hdr_fallback_e::direct_surface_missing;
        }
        else {
          return { dynamic_hdr_format_e::dolby_vision_profile_84, dynamic_hdr_fallback_e::none };
        }
      }
      else {
        dv_fallback = dynamic_hdr_fallback_e::colorspace_unsupported;
      }

      // The reason is only worth reporting when the client actually asked
      // for DV; an automatic preference losing an unavailable option is not
      // a diagnostic the client can act on.
      const dynamic_hdr_fallback_e reported =
        dv_requested ? *dv_fallback : dynamic_hdr_fallback_e::none;

      if (request.preference == dynamic_hdr_preference_e::hdr10_only) {
        return { dynamic_hdr_format_e::none, reported };
      }

      // HDR10+ needs PQ and a carriage (HEVC or AV1 — formats_for() gates
      // the same way). A legacy client that reported nothing keeps the
      // unconditional HDR10+ of previous Sunshine versions.
      const bool hdr10_plus_capable =
        !request.caps_reported || (request.caps_mask & DYNAMIC_HDR_CAPS_HDR10_PLUS) != 0;
      const bool carriable = gates.video_format == 1 || gates.video_format == 2;
      if (gates.dynamic_range_mode == 1 && carriable && hdr10_plus_capable) {
        return { dynamic_hdr_format_e::hdr10_plus, reported };
      }
      return { dynamic_hdr_format_e::none, reported };
    }

    // Non-DV preference: a DV-capable client that chose otherwise gets the
    // preference as the reason, for client-side "you disabled this" UI.
    const dynamic_hdr_fallback_e preference_reason =
      (request.caps_mask & (DYNAMIC_HDR_CAPS_DOLBY_VISION_81 | DYNAMIC_HDR_CAPS_DOLBY_VISION_84)) != 0
        ? dynamic_hdr_fallback_e::preference
        : dynamic_hdr_fallback_e::none;

    if (request.preference == dynamic_hdr_preference_e::hdr10_only) {
      return { dynamic_hdr_format_e::none, preference_reason };
    }

    const bool hdr10_plus_capable =
      !request.caps_reported || (request.caps_mask & DYNAMIC_HDR_CAPS_HDR10_PLUS) != 0;
    const bool carriable = gates.video_format == 1 || gates.video_format == 2;
    if (gates.dynamic_range_mode == 1 && carriable && hdr10_plus_capable) {
      return { dynamic_hdr_format_e::hdr10_plus, preference_reason };
    }
    return { dynamic_hdr_format_e::none, preference_reason };
  }

  dynamic_hdr_request_t
  parse_dynamic_hdr_request(
    std::optional<std::string_view> caps,
    std::optional<std::string_view> dolby_vision_direct_surface,
    std::optional<std::string_view> preference) noexcept {
    dynamic_hdr_request_t request;

    // A well-formed caps argument switches the client into negotiated mode;
    // anything wrong with it leaves the legacy behavior in place rather than
    // downgrading the client behind its back. Bits this host does not know
    // are masked off, not fatal: the report survives with the subset this
    // host understands (an explicit 0 stays a valid "no formats" report).
    if (caps) {
      if (const auto mask = parse_uint(*caps)) {
        constexpr std::uint64_t known_bits = DYNAMIC_HDR_CAPS_HDR10_PLUS |
                                             DYNAMIC_HDR_CAPS_VIVID_PQ |
                                             DYNAMIC_HDR_CAPS_VIVID_HLG |
                                             DYNAMIC_HDR_CAPS_DOLBY_VISION_81 |
                                             DYNAMIC_HDR_CAPS_DOLBY_VISION_84;
        request.caps_mask = static_cast<std::uint32_t>(*mask & known_bits);
        request.caps_reported = true;
      }
    }

    if (dolby_vision_direct_surface) {
      if (const auto surface = parse_uint(*dolby_vision_direct_surface)) {
        request.dolby_vision_direct_surface = *surface != 0;
      }
    }

    if (preference) {
      switch (parse_uint(*preference).value_or(0)) {
        case 1:
          request.preference = dynamic_hdr_preference_e::dolby_vision;
          break;
        case 2:
          request.preference = dynamic_hdr_preference_e::hdr10_plus;
          break;
        case 3:
          request.preference = dynamic_hdr_preference_e::hdr10_only;
          break;
        default:
          break;
      }
    }

    return request;
  }

  std::string_view
  to_string(dynamic_hdr_format_e format) noexcept {
    switch (format) {
      case dynamic_hdr_format_e::none:
        return "none";
      case dynamic_hdr_format_e::hdr10_plus:
        return "hdr10_plus";
      case dynamic_hdr_format_e::vivid_pq:
        return "vivid_pq";
      case dynamic_hdr_format_e::vivid_hlg:
        return "vivid_hlg";
      case dynamic_hdr_format_e::dolby_vision_profile_81:
        return "dolby_vision_profile_81";
      case dynamic_hdr_format_e::dolby_vision_profile_84:
        return "dolby_vision_profile_84";
    }
    return "unknown";
  }

  std::string_view
  to_string(dynamic_hdr_fallback_e fallback) noexcept {
    switch (fallback) {
      case dynamic_hdr_fallback_e::none:
        return "none";
      case dynamic_hdr_fallback_e::codec_unsupported:
        return "codec_unsupported";
      case dynamic_hdr_fallback_e::colorspace_unsupported:
        return "colorspace_unsupported";
      case dynamic_hdr_fallback_e::client_caps_missing:
        return "client_caps_missing";
      case dynamic_hdr_fallback_e::direct_surface_missing:
        return "direct_surface_missing";
      case dynamic_hdr_fallback_e::preference:
        return "preference";
    }
    return "unknown";
  }

}  // namespace hdr
