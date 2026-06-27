/**
 * @file src/file_mapping_config.cpp
 * @brief Parse file mapping configuration into runtime mappings.
 */
#include "file_mapping_config.h"

#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace file_mapping_config {
  namespace {
    std::optional<file_mapping::access_mode_e>
    parse_mode(const nlohmann::json &item, const std::string &prefix, std::vector<std::string> &warnings) {
      if (!item.contains("mode")) {
        return file_mapping::access_mode_e::read;
      }
      if (!item["mode"].is_string()) {
        warnings.push_back(prefix + "mode must be a string");
        return std::nullopt;
      }
      const auto mode = item["mode"].get<std::string>();
      return mode == "readwrite" ? file_mapping::access_mode_e::readwrite : file_mapping::access_mode_e::read;
    }

    std::vector<std::string>
    parse_clients(const nlohmann::json &item) {
      std::vector<std::string> clients;
      if (!item.contains("clients") || !item["clients"].is_array()) {
        return clients;
      }

      for (const auto &client : item["clients"]) {
        if (client.is_string()) {
          clients.push_back(client.get<std::string>());
        }
      }
      return clients;
    }

    bool
    read_optional_bool(const nlohmann::json &item, const char *name, bool fallback, bool &out, const std::string &prefix, std::vector<std::string> &warnings) {
      if (!item.contains(name)) {
        out = fallback;
        return true;
      }
      if (!item[name].is_boolean()) {
        warnings.push_back(prefix + name + " must be a boolean");
        return false;
      }
      out = item[name].get<bool>();
      return true;
    }

    bool
    read_optional_uintmax(const nlohmann::json &item, const char *name, std::uintmax_t fallback, std::uintmax_t &out, const std::string &prefix, std::vector<std::string> &warnings) {
      if (!item.contains(name)) {
        out = fallback;
        return true;
      }
      std::uint64_t value = 0;
      if (item[name].is_number_unsigned()) {
        value = item[name].get<std::uint64_t>();
      }
      else if (item[name].is_number_integer()) {
        const auto signed_value = item[name].get<std::int64_t>();
        if (signed_value < 0) {
          warnings.push_back(prefix + name + " must be a non-negative integer");
          return false;
        }
        value = static_cast<std::uint64_t>(signed_value);
      }
      else {
        warnings.push_back(prefix + name + " must be a non-negative integer");
        return false;
      }
      if (value > std::numeric_limits<std::uintmax_t>::max()) {
        warnings.push_back(prefix + name + " is too large");
        return false;
      }
      out = static_cast<std::uintmax_t>(value);
      return true;
    }

    std::string
    item_prefix(std::size_t index) {
      return "file_mappings[" + std::to_string(index) + "]: ";
    }

    std::optional<std::string>
    base64_decode(std::string_view text) {
      auto decode_char = [](unsigned char ch) -> int {
        if (ch >= 'A' && ch <= 'Z') return ch - 'A';
        if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
        if (ch >= '0' && ch <= '9') return ch - '0' + 52;
        if (ch == '+') return 62;
        if (ch == '/') return 63;
        return -1;
      };

      if (text.size() % 4 != 0) {
        return std::nullopt;
      }

      std::string out;
      out.reserve((text.size() / 4) * 3);
      for (std::size_t i = 0; i < text.size(); i += 4) {
        const auto c0 = decode_char(static_cast<unsigned char>(text[i]));
        const auto c1 = decode_char(static_cast<unsigned char>(text[i + 1]));
        const auto c2 = text[i + 2] == '=' ? -2 : decode_char(static_cast<unsigned char>(text[i + 2]));
        const auto c3 = text[i + 3] == '=' ? -2 : decode_char(static_cast<unsigned char>(text[i + 3]));
        if (c0 < 0 || c1 < 0 || c2 == -1 || c3 == -1 || (c2 == -2 && c3 != -2)) {
          return std::nullopt;
        }

        out.push_back(static_cast<char>((c0 << 2) | (c1 >> 4)));
        if (c2 != -2) {
          out.push_back(static_cast<char>(((c1 & 0x0f) << 4) | (c2 >> 2)));
        }
        if (c3 != -2) {
          out.push_back(static_cast<char>(((c2 & 0x03) << 6) | c3));
        }
      }
      return out;
    }

    std::optional<std::string>
    decode_config_value(const std::string &text, std::vector<std::string> &warnings) {
      constexpr std::string_view prefix = "base64:";
      if (text.rfind(prefix, 0) != 0) {
        return text;
      }

      auto decoded = base64_decode(std::string_view { text }.substr(prefix.size()));
      if (!decoded) {
        warnings.emplace_back("file_mappings: invalid base64 encoded JSON");
      }
      return decoded;
    }
  }  // namespace

  parse_result_t
  parse_mappings_json(const std::string &json_text) {
    parse_result_t result;
    if (json_text.empty()) {
      return result;
    }

    auto decoded_text = decode_config_value(json_text, result.warnings);
    if (!decoded_text) {
      return result;
    }

    nlohmann::json root;
    try {
      root = nlohmann::json::parse(*decoded_text);
    } catch (const nlohmann::json::exception &e) {
      result.warnings.push_back(std::string { "file_mappings: invalid JSON: " } + e.what());
      return result;
    }

    if (!root.is_array()) {
      result.warnings.emplace_back("file_mappings: expected a JSON array");
      return result;
    }

    for (std::size_t index = 0; index < root.size(); ++index) {
      const auto &item = root[index];
      const auto prefix = item_prefix(index);
      if (!item.is_object()) {
        result.warnings.push_back(prefix + "expected object");
        continue;
      }
      if (!item.contains("id") || !item["id"].is_string() || !file_mapping::is_valid_mapping_id(item["id"].get<std::string>())) {
        result.warnings.push_back(prefix + "missing or invalid id");
        continue;
      }
      if (!item.contains("path") || !item["path"].is_string()) {
        result.warnings.push_back(prefix + "missing path");
        continue;
      }

      file_mapping::mapping_t mapping;
      mapping.id = item["id"].get<std::string>();
      if (item.contains("name")) {
        if (!item["name"].is_string() || item["name"].get<std::string>().empty()) {
          result.warnings.push_back(prefix + "name must be a non-empty string");
          continue;
        }
        mapping.name = item["name"].get<std::string>();
      }
      else {
        mapping.name = mapping.id;
      }
      mapping.local_root = item["path"].get<std::string>();
      auto mode = parse_mode(item, prefix, result.warnings);
      if (!mode) {
        continue;
      }
      mapping.mode = *mode;
      if (!read_optional_bool(item, "allow_delete", false, mapping.allow_delete, prefix, result.warnings) ||
          !read_optional_bool(item, "allow_execute", false, mapping.allow_execute, prefix, result.warnings) ||
          !read_optional_bool(item, "follow_reparse_points", false, mapping.follow_reparse_points, prefix, result.warnings) ||
          !read_optional_uintmax(item, "max_file_size", 0, mapping.max_file_size, prefix, result.warnings)) {
        continue;
      }
      mapping.clients = parse_clients(item);
      if (mapping.mode == file_mapping::access_mode_e::readwrite) {
        result.warnings.push_back(prefix + "readwrite mode ignored in read-only phase");
        mapping.mode = file_mapping::access_mode_e::read;
      }
      if (mapping.allow_delete) {
        result.warnings.push_back(prefix + "allow_delete ignored in read-only phase");
        mapping.allow_delete = false;
      }
      if (mapping.allow_execute) {
        result.warnings.push_back(prefix + "allow_execute ignored");
        mapping.allow_execute = false;
      }
      if (mapping.follow_reparse_points) {
        result.warnings.push_back(prefix + "follow_reparse_points ignored");
        mapping.follow_reparse_points = false;
      }

      std::error_code ec;
      if (!std::filesystem::is_directory(mapping.local_root, ec)) {
        result.warnings.push_back(prefix + "path is not an existing directory");
        continue;
      }

      result.mappings.push_back(std::move(mapping));
    }

    return result;
  }
}  // namespace file_mapping_config
