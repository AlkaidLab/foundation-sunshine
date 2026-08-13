/**
 * @file src/hdr/client_display_capabilities.cpp
 * @brief Validation for HDR display capabilities reported by a client.
 */
#include "client_display_capabilities.h"

#include <charconv>
#include <cmath>

#include <nlohmann/json.hpp>

namespace hdr {
  namespace {
    bool
    parse_finite_float(std::string_view text, float &value) noexcept {
      if (text.empty()) {
        return false;
      }

      const char *begin = text.data();
      const char *end = begin + text.size();
      const auto [position, error] = std::from_chars(begin, end, value, std::chars_format::general);
      return error == std::errc {} && position == end && std::isfinite(value);
    }

    effective_target_t
    base_target(const client_display_capabilities_t &reported) {
      effective_target_t result;
      result.capabilities = reported;
      result.source = reported.reported ? target_source_e::client_report : target_source_e::safe_defaults;
      return result;
    }

    bool
    read_manual_number(const nlohmann::json &client, const char *field, float &value) {
      const auto it = client.find(field);
      if (it == client.end() || !it->is_number()) {
        return false;
      }
      value = it->get<float>();
      return std::isfinite(value);
    }

    bool
    valid_luminance_values(float max_nits, float min_nits, float max_full_frame_nits) noexcept {
      constexpr float maximum_supported_nits = 10000.0f;
      constexpr float maximum_supported_black_level_nits = 100.0f;
      return std::isfinite(max_nits) && std::isfinite(min_nits) && std::isfinite(max_full_frame_nits) &&
             max_nits >= 1.0f && max_nits <= maximum_supported_nits &&
             min_nits >= 0.0f && min_nits <= maximum_supported_black_level_nits &&
             max_full_frame_nits >= 1.0f && max_full_frame_nits <= maximum_supported_nits &&
             min_nits <= max_full_frame_nits && max_full_frame_nits <= max_nits;
    }
  }  // namespace

  client_display_capabilities_parse_result_t
  validate_client_display_capabilities(
    float max_nits,
    float min_nits,
    float max_full_frame_nits
  ) noexcept {
    client_display_capabilities_parse_result_t result;
    if (!valid_luminance_values(max_nits, min_nits, max_full_frame_nits)) {
      result.fallback_reason = "HDR luminance values are out-of-range or inconsistent";
      return result;
    }

    result.capabilities = { true, max_nits, min_nits, max_full_frame_nits };
    return result;
  }

  client_display_capabilities_parse_result_t
  parse_client_display_capabilities(
    std::optional<std::string_view> max_nits,
    std::optional<std::string_view> min_nits,
    std::optional<std::string_view> max_full_frame_nits
  ) noexcept {
    client_display_capabilities_parse_result_t result;

    const unsigned present_count = static_cast<unsigned>(max_nits.has_value()) +
                                   static_cast<unsigned>(min_nits.has_value()) +
                                   static_cast<unsigned>(max_full_frame_nits.has_value());
    if (present_count == 0) {
      return result;
    }
    if (present_count != 3) {
      result.fallback_reason = "client supplied an incomplete HDR luminance report";
      return result;
    }

    float parsed_max {};
    float parsed_min {};
    float parsed_max_full_frame {};
    if (!parse_finite_float(*max_nits, parsed_max) ||
        !parse_finite_float(*min_nits, parsed_min) ||
        !parse_finite_float(*max_full_frame_nits, parsed_max_full_frame)) {
      result.fallback_reason = "client supplied a malformed HDR luminance report";
      return result;
    }

    const auto validated = validate_client_display_capabilities(parsed_max, parsed_min, parsed_max_full_frame);
    if (!validated.capabilities.reported) {
      result.fallback_reason = "client supplied an out-of-range or inconsistent HDR luminance report";
      return result;
    }
    return validated;
  }

  effective_target_t
  resolve_effective_target(
    std::string_view clients_json,
    std::string_view client_uuid,
    std::string_view legacy_name,
    const client_display_capabilities_t &reported_capabilities
  ) noexcept {
    auto result = base_target(reported_capabilities);
    try {
      const auto clients = nlohmann::json::parse(clients_json);
      if (!clients.is_array()) {
        result.error = "clients: expected a JSON array";
        return result;
      }

      const nlohmann::json *match = nullptr;
      if (!client_uuid.empty()) {
        for (const auto &client : clients) {
          if (!client.is_object()) continue;
          const auto uuid = client.find("uuid");
          if (uuid == client.end() || !uuid->is_string() || uuid->get_ref<const std::string &>() != client_uuid) continue;
          if (match) {
            result.error = "clients: duplicate uuid";
            return result;
          }
          match = &client;
        }
      }
      else if (!legacy_name.empty()) {
        for (const auto &client : clients) {
          if (!client.is_object()) continue;
          const auto name = client.find("name");
          if (name == client.end() || !name->is_string() || name->get_ref<const std::string &>() != legacy_name) continue;
          if (match) {
            result.error = "clients: legacy client name is ambiguous";
            return result;
          }
          match = &client;
          result.used_legacy_name = true;
        }
      }

      if (!match) return result;
      const auto mode = match->find("hdrBrightnessMode");
      if (mode == match->end() || (mode->is_string() && mode->get_ref<const std::string &>() == "auto")) {
        return result;
      }
      if (!mode->is_string() || mode->get_ref<const std::string &>() != "manual") {
        result.error = "hdrBrightnessMode must be 'auto' or 'manual'";
        return result;
      }

      float max_nits {};
      float min_nits {};
      float max_full_frame_nits {};
      if (!read_manual_number(*match, "hdrBrightnessMaxNits", max_nits) ||
          !read_manual_number(*match, "hdrBrightnessMinNits", min_nits) ||
          !read_manual_number(*match, "hdrBrightnessMaxFullFrameNits", max_full_frame_nits)) {
        result.error = "manual HDR brightness requires three finite numeric values";
        return result;
      }

      const auto validated = validate_client_display_capabilities(max_nits, min_nits, max_full_frame_nits);
      if (!validated.capabilities.reported) {
        result.error = validated.fallback_reason;
        return result;
      }

      result.capabilities = validated.capabilities;
      result.source = target_source_e::manual_override;
      return result;
    }
    catch (const std::exception &error) {
      result.error = std::string { "clients: invalid configuration: " } + error.what();
      return result;
    }
    catch (...) {
      result.error = "clients: invalid configuration";
      return result;
    }
  }

  std::string_view
  to_string(target_source_e source) noexcept {
    switch (source) {
      case target_source_e::client_report:
        return "client_report";
      case target_source_e::manual_override:
        return "manual_override";
      case target_source_e::windows_hdr_calibration:
        return "windows_hdr_calibration";
      case target_source_e::safe_defaults:
        return "safe_defaults";
    }
    return "safe_defaults";
  }
}  // namespace hdr
