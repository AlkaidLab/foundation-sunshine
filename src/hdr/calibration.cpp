/**
 * @file src/hdr/calibration.cpp
 * @brief ICC MHC2 parsing and persisted calibration validation.
 */
#include "calibration.h"
#include "src/display_device/color_profile.h"

#include <algorithm>
#include <bit>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace hdr {
  namespace {
    std::uint32_t
    u32(std::span<const std::uint8_t> bytes, std::size_t offset) {
      return (std::uint32_t { bytes[offset] } << 24) |
             (std::uint32_t { bytes[offset + 1] } << 16) |
             (std::uint32_t { bytes[offset + 2] } << 8) |
             std::uint32_t { bytes[offset + 3] };
    }

    float
    fixed16(std::span<const std::uint8_t> bytes, std::size_t offset) {
      return static_cast<float>(std::bit_cast<std::int32_t>(u32(bytes, offset))) / 65536.0f;
    }

    std::optional<std::span<const std::uint8_t>>
    tag(std::span<const std::uint8_t> bytes, std::uint32_t signature) {
      std::optional<std::span<const std::uint8_t>> found;
      const auto count = u32(bytes, 128);
      for (std::size_t index = 0; index < count; ++index) {
        const auto entry = 132 + index * 12;
        const auto offset = u32(bytes, entry + 4);
        const auto size = u32(bytes, entry + 8);
        if (offset < 132 + count * 12 || offset % 4 != 0 ||
            offset > bytes.size() || size > bytes.size() - offset) return std::nullopt;
        if (u32(bytes, entry) == signature) {
          if (found) return std::nullopt;
          found = bytes.subspan(offset, size);
        }
      }
      return found;
    }
  }  // namespace

  std::optional<client_display_capabilities_t>
  parse_calibration_luminance(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 132 || bytes.size() > 16 * 1024 * 1024 ||
        u32(bytes, 0) != bytes.size() || (bytes[8] != 2 && bytes[8] != 4) ||
        u32(bytes, 12) != 0x6D6E7472 ||  // mntr
        u32(bytes, 16) != 0x52474220 ||  // RGB
        u32(bytes, 20) != 0x58595A20 ||  // XYZ
        u32(bytes, 36) != 0x61637370 ||  // acsp
        u32(bytes, 128) > (bytes.size() - 132) / 12) return std::nullopt;

    const auto mhc2 = tag(bytes, 0x4D484332);
    const auto lumi = tag(bytes, 0x6C756D69);
    if (!mhc2 || mhc2->size() < 36 || u32(*mhc2, 0) != 0x4D484332 || u32(*mhc2, 4) != 0 ||
        !lumi || lumi->size() != 20 || u32(*lumi, 0) != 0x58595A20 || u32(*lumi, 4) != 0) return std::nullopt;

    const auto count = u32(*mhc2, 8);
    const auto matrix = u32(*mhc2, 20);
    if (count > 4096 || (matrix && (matrix < 36 || matrix % 4 || matrix > mhc2->size() || mhc2->size() - matrix < 48))) return std::nullopt;
    for (const auto channel : { 24u, 28u, 32u }) {
      const auto offset = u32(*mhc2, channel);
      if (count == 0) {
        if (offset != 0) return std::nullopt;
      }
      else if (count < 2 || offset < 36 || offset % 4 || offset > mhc2->size() ||
               mhc2->size() - offset < 8 + count * 4 ||
               u32(*mhc2, offset) != 0x73663332 || u32(*mhc2, offset + 4) != 0)
        return std::nullopt;
    }
    const auto validated = validate_client_display_capabilities(
      fixed16(*mhc2, 16), fixed16(*mhc2, 12), fixed16(*lumi, 12));
    if (!validated.capabilities.reported) return std::nullopt;
    return validated.capabilities;
  }

  bool
  same_calibration(const calibration_profile_t &left, const calibration_profile_t &right) {
    return left.profile_name == right.profile_name && left.fingerprint == right.fingerprint;
  }

  bool
  calibration_matches(const calibration_binding_t &binding, std::string_view user_sid,
    std::string_view vdd_id, const calibration_profile_t &profile) {
    return binding.user_sid == user_sid && binding.vdd_id == vdd_id && same_calibration(binding.profile, profile);
  }

  void
  to_json(nlohmann::json &json, const calibration_binding_t &binding) {
    json = {
      { "version", 1 }, { "userSid", binding.user_sid }, { "vddId", binding.vdd_id },
      { "profileName", binding.profile.profile_name }, { "sha256", binding.profile.fingerprint }
    };
  }

  void
  from_json(const nlohmann::json &json, calibration_binding_t &binding) {
    if (json.at("version").get<int>() != 1) throw std::invalid_argument("Unsupported HDR calibration version");
    binding.user_sid = json.at("userSid").get<std::string>();
    binding.vdd_id = json.at("vddId").get<std::string>();
    binding.profile.profile_name = json.at("profileName").get<std::string>();
    binding.profile.fingerprint = json.at("sha256").get<std::string>();
    if (binding.user_sid.empty() || binding.vdd_id.empty() ||
        !display_device::color_profile::is_valid_profile_basename(binding.profile.profile_name) ||
        binding.profile.fingerprint.size() != 64 ||
        !std::all_of(binding.profile.fingerprint.begin(), binding.profile.fingerprint.end(), [](char c) {
          return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        })) throw std::invalid_argument("Invalid HDR calibration binding");
  }

  std::optional<calibration_binding_t>
  client_calibration(std::string_view clients, std::string_view uuid) {
    if (uuid.empty()) return std::nullopt;
    try {
      const auto entries = nlohmann::json::parse(clients);
      if (!entries.is_array()) return std::nullopt;
      const nlohmann::json *match = nullptr;
      for (const auto &entry : entries) {
        if (!entry.is_object() || entry.value("uuid", std::string {}) != uuid) continue;
        if (match) return std::nullopt;
        match = &entry;
      }
      if (match && match->contains("hdrCalibration")) return match->at("hdrCalibration").get<calibration_binding_t>();
    }
    catch (const std::exception &) {
      // An invalid or stale binding must never turn into another client's calibration.
    }
    return std::nullopt;
  }

  std::string
  preserve_client_calibrations(std::string_view incoming, std::string_view existing) {
    auto clients = nlohmann::json::parse(incoming);
    if (!clients.is_array()) throw std::invalid_argument("Client settings must be an array");
    for (auto &client : clients) {
      if (!client.is_object()) continue;
      client.erase("hdrCalibration");
      if (const auto binding = client_calibration(existing, client.value("uuid", std::string {}))) {
        client["hdrCalibration"] = *binding;
      }
    }
    return clients.dump();
  }
}  // namespace hdr
