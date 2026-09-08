/**
 * @file src/display_device/color_profile.cpp
 * @brief Resolve per-client HDR color profile configuration.
 */
#include "color_profile.h"

#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace display_device::color_profile {
  namespace {
    using json = nlohmann::json;

    struct client_entry_t {
      std::optional<std::string> uuid;
      std::optional<std::string> name;
      std::optional<std::string> hdr_profile;
      bool has_hdr_profile { false };
      std::size_t index {};
    };

    std::string
    entry_prefix(std::size_t index) {
      return "clients[" + std::to_string(index) + "]: ";
    }

    bool
    read_optional_string(
      const json &item,
      const char *field,
      std::optional<std::string> &output,
      std::string &error,
      std::size_t index
    ) {
      const auto it = item.find(field);
      if (it == item.end()) {
        return true;
      }
      if (!it->is_string()) {
        error = entry_prefix(index) + field + " must be a string";
        return false;
      }

      output = it->get<std::string>();
      return true;
    }

    bool
    has_supported_extension(std::string_view profile) noexcept {
      if (profile.size() <= 4 || profile[profile.size() - 4] != '.') {
        return false;
      }

      const auto ascii_lower = [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
      };
      return ascii_lower(static_cast<unsigned char>(profile[profile.size() - 3])) == 'i' &&
             ascii_lower(static_cast<unsigned char>(profile[profile.size() - 2])) == 'c' &&
             (ascii_lower(static_cast<unsigned char>(profile[profile.size() - 1])) == 'c' ||
              ascii_lower(static_cast<unsigned char>(profile[profile.size() - 1])) == 'm');
    }

    resolve_result_t
    resolve_profile(const client_entry_t &entry, bool used_legacy_name) {
      resolve_result_t result;
      result.used_legacy_name = used_legacy_name;

      if (!entry.has_hdr_profile) {
        return result;
      }

      const auto &profile = *entry.hdr_profile;
      if (profile.empty()) {
        result.policy = profile_policy_e::clear;
        return result;
      }

      if (!is_valid_profile_basename(profile)) {
        result.error = entry_prefix(entry.index) +
                       "hdrProfile must be a basename with a .icc or .icm extension";
        return result;
      }

      result.policy = profile_policy_e::apply;
      result.profile = profile;
      return result;
    }
  }  // namespace

  bool
  is_valid_profile_basename(std::string_view profile) noexcept {
    if (!has_supported_extension(profile)) {
      return false;
    }

    // These include path separators, quotes, control characters, and the
    // characters Windows does not permit in a file basename.
    constexpr std::string_view forbidden { R"(<>:"/\|?*')" };
    for (const unsigned char value : profile) {
      if (value < 0x20 || value == 0x7f || forbidden.find(static_cast<char>(value)) != std::string_view::npos) {
        return false;
      }
    }

    return profile.front() != ' ' && profile.back() != ' ' && profile.back() != '.';
  }

  resolve_result_t
  resolve_client_hdr_profile(
    std::string_view clients_json,
    std::string_view client_uuid,
    std::string_view legacy_name
  ) {
    resolve_result_t result;

    json root;
    try {
      root = json::parse(clients_json);
    }
    catch (const json::exception &e) {
      result.error = std::string { "clients: invalid JSON: " } + e.what();
      return result;
    }

    if (!root.is_array()) {
      result.error = "clients: expected a JSON array";
      return result;
    }

    std::vector<client_entry_t> entries;
    entries.reserve(root.size());
    for (std::size_t index = 0; index < root.size(); ++index) {
      const auto &item = root[index];
      if (!item.is_object()) {
        result.error = entry_prefix(index) + "expected an object";
        return result;
      }

      client_entry_t entry;
      entry.index = index;
      if (!read_optional_string(item, "uuid", entry.uuid, result.error, index) ||
          !read_optional_string(item, "name", entry.name, result.error, index)) {
        return result;
      }

      entry.has_hdr_profile = item.contains("hdrProfile");
      if (!read_optional_string(item, "hdrProfile", entry.hdr_profile, result.error, index)) {
        return result;
      }
      entries.push_back(std::move(entry));
    }

    if (!client_uuid.empty()) {
      const client_entry_t *match = nullptr;
      for (const auto &entry : entries) {
        if (!entry.uuid || *entry.uuid != client_uuid) {
          continue;
        }
        if (match) {
          result.error = "clients: duplicate uuid '" + std::string { client_uuid } + "'";
          return result;
        }
        match = &entry;
      }

      return match ? resolve_profile(*match, false) : result;
    }

    if (legacy_name.empty()) {
      return result;
    }

    const client_entry_t *match = nullptr;
    for (const auto &entry : entries) {
      if (!entry.name || *entry.name != legacy_name) {
        continue;
      }
      if (match) {
        result.error = "clients: legacy name '" + std::string { legacy_name } + "' is ambiguous";
        return result;
      }
      match = &entry;
    }

    return match ? resolve_profile(*match, true) : result;
  }
}  // namespace display_device::color_profile
