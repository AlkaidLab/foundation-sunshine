/**
 * @file src/hdr/dynamic_hdr_selection.h
 * @brief Negotiation of the dynamic HDR format for a streaming session.
 *
 * Dolby Vision Profile 8.1 must not be delivered unrequested: a client that
 * has not configured a video/dolby-vision decoder may mishandle the UNSPEC 62
 * NAL. The client therefore reports its capabilities with the SDP of the
 * RTSP ANNOUNCE, the host decides once per session (docs/dolby_vision_
 * profile81.md §2.2, §4), and the decision rides back in the ANNOUNCE
 * response headers.
 *
 * HDR10+ and HDR Vivid are deliberately NOT gated by this negotiation for
 * legacy clients: Sunshine has always emitted them whenever the stream could
 * carry them, and a client that sends no capability report keeps that
 * behavior. Only a client that does report capabilities gets them honored —
 * including a negotiated downgrade to plain HDR10.
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace hdr {

  /// Capability bits the client reports in x-ss-video[0].dynamicHdrCaps.
  enum dynamic_hdr_caps_e : std::uint32_t {
    DYNAMIC_HDR_CAPS_NONE = 0,
    DYNAMIC_HDR_CAPS_HDR10_PLUS = 1u << 0,
    DYNAMIC_HDR_CAPS_VIVID_PQ = 1u << 1,
    DYNAMIC_HDR_CAPS_VIVID_HLG = 1u << 2,
    DYNAMIC_HDR_CAPS_DOLBY_VISION_81 = 1u << 3,
    DYNAMIC_HDR_CAPS_DOLBY_VISION_84 = 1u << 4,
  };

  /// The format the host selected, reported as X-SS-Dynamic-HDR.
  /// Values are wire-stable: do not renumber.
  enum class dynamic_hdr_format_e : int {
    none = 0,  ///< Plain HDR10 (or SDR)
    hdr10_plus = 1,
    vivid_pq = 2,
    vivid_hlg = 3,
    dolby_vision_profile_81 = 4,
    dolby_vision_profile_84 = 5,  ///< DV with an HLG base layer
  };

  /// The client-side user preference, x-ss-video[0].dynamicHdrPreference.
  enum class dynamic_hdr_preference_e : int {
    automatic = 0,
    dolby_vision = 1,
    hdr10_plus = 2,
    hdr10_only = 3,
  };

  /// Why Dolby Vision was not selected when the client asked for it,
  /// reported as X-SS-Dynamic-HDR-Fallback for client-side diagnostics.
  enum class dynamic_hdr_fallback_e {
    none,
    codec_unsupported,  ///< Dolby Vision requires HEVC
    colorspace_unsupported,  ///< 8.1 requires BT.2020 PQ; 8.4 requires BT.2020 HLG (or is excluded by an active pre-encode filter)
    client_caps_missing,  ///< Client reported no Dolby Vision capability
    direct_surface_missing,  ///< Client cannot promise a direct surface path
    preference,  ///< A non-DV preference won
  };

  /// What the client asked for, parsed from the ANNOUNCE SDP.
  struct dynamic_hdr_request_t {
    /// dynamic_hdr_caps_e bits. Zero with reported == false means a legacy
    /// client that sent nothing and keeps today's unconditional behavior.
    std::uint32_t caps_mask = 0;
    bool caps_reported = false;
    bool dolby_vision_direct_surface = false;
    dynamic_hdr_preference_e preference = dynamic_hdr_preference_e::automatic;
  };

  /// Host-side gates known at ANNOUNCE time.
  struct dynamic_hdr_host_gates_t {
    /// config_t::videoFormat convention: 0 H.264, 1 HEVC, 2 AV1.
    int video_format = 0;
    /// x-nv-video[0].dynamicRangeMode: 0 SDR, 1 PQ, 2 HLG.
    int dynamic_range_mode = 0;
    /// The app has the SDR-to-HDR (RTX HDR) feature enabled. Its pipeline is
    /// PQ-pinned, which excludes the HLG-base profile 8.4 for the whole app —
    /// even when the filter itself stays off because the client requested HLG
    /// (docs/dolby_vision_profile84.md §2).
    bool synthetic_hdr_enabled = false;
  };

  struct dynamic_hdr_selection_t {
    dynamic_hdr_format_e format = dynamic_hdr_format_e::none;
    /// Populated when the client is DV-capable or asked for DV but DV was not
    /// selected; none otherwise. Reported to the client for diagnostics.
    dynamic_hdr_fallback_e fallback_reason = dynamic_hdr_fallback_e::none;

    bool
    dolby_vision_active() const {
      return format == dynamic_hdr_format_e::dolby_vision_profile_81 ||
             format == dynamic_hdr_format_e::dolby_vision_profile_84;
    }
  };

  /**
   * The one-shot session decision, docs/dolby_vision_profile81.md §4 and
   * docs/dolby_vision_profile84.md §3.1.
   *
   * DV priority: 8.1 whenever the client reports it and the session is PQ;
   * 8.4 only when the client reports 8.4 alone and the session is HLG
   * (user-confirmed rule). Preference: dolby_vision/automatic → DV →
   * HDR10+ → HDR10; hdr10_plus → HDR10+ → HDR10; hdr10_only → HDR10.
   * DV additionally requires HEVC and the client capability with a direct
   * surface. HDR10+ requires PQ; a client that reported capabilities without
   * the HDR10+ bit gets plain HDR10, while a legacy client (no report) keeps
   * the unconditional HDR10+ of previous versions.
   */
  [[nodiscard]] dynamic_hdr_selection_t
  select_dynamic_hdr(
    const dynamic_hdr_request_t &request,
    const dynamic_hdr_host_gates_t &gates) noexcept;

  /**
   * Parse the three SDP arguments. Missing or malformed values fall back to
   * the legacy defaults — caps_reported stays false unless the caps argument
   * itself is present and well-formed, so one bad field cannot flip a legacy
   * client into a negotiated downgrade. Unknown capability bits are masked
   * off while keeping the report valid: a future client must not lose its
   * whole negotiation because this host does not know the newest format.
   */
  [[nodiscard]] dynamic_hdr_request_t
  parse_dynamic_hdr_request(
    std::optional<std::string_view> caps,
    std::optional<std::string_view> dolby_vision_direct_surface,
    std::optional<std::string_view> preference) noexcept;

  [[nodiscard]] std::string_view
  to_string(dynamic_hdr_format_e format) noexcept;

  [[nodiscard]] std::string_view
  to_string(dynamic_hdr_fallback_e fallback) noexcept;

  /// Numeric value for the X-SS-Dynamic-HDR response header.
  [[nodiscard]] constexpr int
  to_wire(dynamic_hdr_format_e format) noexcept {
    return static_cast<int>(format);
  }

}  // namespace hdr
