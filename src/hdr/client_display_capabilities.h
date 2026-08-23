/**
 * @file src/hdr/client_display_capabilities.h
 * @brief Validated HDR display capabilities reported by a streaming client.
 */
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace hdr {
  struct client_display_capabilities_t {
    static constexpr float default_max_nits = 1000.0f;
    static constexpr float default_min_nits = 0.001f;
    static constexpr float default_max_full_frame_nits = 1000.0f;

    bool reported { false };
    float max_nits { default_max_nits };
    float min_nits { default_min_nits };
    float max_full_frame_nits { default_max_full_frame_nits };
    // Client-measured SDR reference white in nits; 0 = client did not report one.
    // Independently parsed (sdrBrightness launch arg), not part of the atomic
    // three-field validation above.
    float sdr_white_nits { 0.0f };
  };

  enum class target_source_e {
    client_report,
    manual_override,
    windows_hdr_calibration,
    safe_defaults,
  };

  struct effective_target_t {
    client_display_capabilities_t capabilities;
    target_source_e source { target_source_e::safe_defaults };
    bool used_legacy_name { false };
    std::string error;

    explicit operator bool() const noexcept {
      return error.empty();
    }
  };

  struct client_display_capabilities_parse_result_t {
    client_display_capabilities_t capabilities;
    std::string fallback_reason;
  };

  /**
   * Parse the three client HDR luminance fields as one atomic capability report.
   * A missing, malformed, or inconsistent field rejects the complete report and
   * returns safe defaults. This prevents mixing reported and fallback values.
   */
  [[nodiscard]] client_display_capabilities_parse_result_t
  parse_client_display_capabilities(
    std::optional<std::string_view> max_nits,
    std::optional<std::string_view> min_nits,
    std::optional<std::string_view> max_full_frame_nits
  ) noexcept;

  /** Validate already-parsed HDR luminance values without changing precision. */
  [[nodiscard]] client_display_capabilities_parse_result_t
  validate_client_display_capabilities(
    float max_nits,
    float min_nits,
    float max_full_frame_nits
  ) noexcept;

  /** Resolve a per-client manual override, otherwise preserve the report/default. */
  [[nodiscard]] effective_target_t
  resolve_effective_target(
    std::string_view clients_json,
    std::string_view client_uuid,
    std::string_view legacy_name,
    const client_display_capabilities_t &reported_capabilities
  ) noexcept;

  [[nodiscard]] std::string_view
  to_string(target_source_e source) noexcept;
}  // namespace hdr
