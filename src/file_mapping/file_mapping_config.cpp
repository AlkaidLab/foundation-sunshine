/**
 * @file src/file_mapping_config.cpp
 * @brief Parse file mapping configuration into runtime mappings.
 */
#include "file_mapping_config.h"

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

namespace file_mapping_config {
  namespace {
    file_mapping::access_mode_e
    parse_mode(const nlohmann::json &item) {
      const auto mode = item.value("mode", std::string { "read" });
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

    std::string
    item_prefix(std::size_t index) {
      return "file_mappings[" + std::to_string(index) + "]: ";
    }
  }  // namespace

  parse_result_t
  parse_mappings_json(const std::string &json_text) {
    parse_result_t result;
    if (json_text.empty()) {
      return result;
    }

    nlohmann::json root;
    try {
      root = nlohmann::json::parse(json_text);
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
      mapping.name = item.value("name", mapping.id);
      mapping.local_root = item["path"].get<std::string>();
      mapping.mode = parse_mode(item);
      mapping.allow_delete = item.value("allow_delete", false);
      mapping.allow_execute = item.value("allow_execute", false);
      mapping.follow_reparse_points = item.value("follow_reparse_points", false);
      mapping.max_file_size = item.value("max_file_size", std::uintmax_t { 0 });
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
