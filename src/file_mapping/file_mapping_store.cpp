/**
 * @file src/file_mapping_store.cpp
 * @brief Thread-safe runtime store for host directory mappings.
 */
#include "file_mapping_store.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <limits>
#include <sstream>
#include <string_view>
#include <system_error>

#include "src/config.h"
#include "src/file_handler.h"

namespace file_mapping_store {
  namespace {
    namespace fs = std::filesystem;

    std::string
    sanitize_id_prefix(std::string text) {
      std::string out;
      out.reserve(text.size());
      bool last_dash = false;
      for (unsigned char ch : text) {
        if (std::isalnum(ch)) {
          out.push_back(static_cast<char>(std::tolower(ch)));
          last_dash = false;
        }
        else if (!last_dash) {
          out.push_back('-');
          last_dash = true;
        }
      }

      while (!out.empty() && out.front() == '-') {
        out.erase(out.begin());
      }
      while (!out.empty() && out.back() == '-') {
        out.pop_back();
      }
      if (out.empty()) {
        out = "folder";
      }
      if (out.size() > 40) {
        out.resize(40);
        while (!out.empty() && out.back() == '-') {
          out.pop_back();
        }
      }
      return out;
    }

    std::string
    short_hash(const fs::path &path) {
      const auto value = std::hash<std::string> {}(path.generic_string());
      std::ostringstream out;
      out << std::hex << (value & 0xffffff);
      return out.str();
    }

    bool
    contains_id(const std::vector<file_mapping::mapping_t> &mappings, const std::string &id) {
      return std::any_of(mappings.begin(), mappings.end(), [&](const file_mapping::mapping_t &mapping) {
        return mapping.id == id;
      });
    }

    bool
    config_file_has_file_mappings_value(const std::string &serialized) {
      try {
        const auto vars = config::parse_config(file_handler::read_file(config::sunshine.config_file.c_str()));
        const auto it = vars.find("file_mappings");
        return it != vars.end() && it->second == serialized;
      }
      catch (...) {
        return false;
      }
    }

    void
    normalize_read_only_mapping(file_mapping::mapping_t &mapping) {
      mapping.mode = file_mapping::access_mode_e::read;
      mapping.allow_delete = false;
      mapping.allow_execute = false;
      mapping.follow_reparse_points = false;
    }

    bool
    read_uintmax_patch_value(const nlohmann::json &json, std::uintmax_t &out) {
      std::uint64_t value = 0;
      if (json.is_number_unsigned()) {
        value = json.get<std::uint64_t>();
      }
      else if (json.is_number_integer()) {
        const auto signed_value = json.get<std::int64_t>();
        if (signed_value < 0) {
          return false;
        }
        value = static_cast<std::uint64_t>(signed_value);
      }
      else {
        return false;
      }
      if (value > std::numeric_limits<std::uintmax_t>::max()) {
        return false;
      }
      out = static_cast<std::uintmax_t>(value);
      return true;
    }

    std::string
    base64_encode(std::string_view text) {
      static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
      std::string out;
      out.reserve(((text.size() + 2) / 3) * 4);

      for (std::size_t i = 0; i < text.size(); i += 3) {
        const auto b0 = static_cast<unsigned char>(text[i]);
        const auto b1 = i + 1 < text.size() ? static_cast<unsigned char>(text[i + 1]) : 0;
        const auto b2 = i + 2 < text.size() ? static_cast<unsigned char>(text[i + 2]) : 0;

        out.push_back(alphabet[(b0 >> 2) & 0x3f]);
        out.push_back(alphabet[((b0 & 0x03) << 4) | ((b1 >> 4) & 0x0f)]);
        out.push_back(i + 1 < text.size() ? alphabet[((b1 & 0x0f) << 2) | ((b2 >> 6) & 0x03)] : '=');
        out.push_back(i + 2 < text.size() ? alphabet[b2 & 0x3f] : '=');
      }

      return out;
    }
  }  // namespace

  void
  store_t::replace(std::vector<file_mapping::mapping_t> mappings) {
    for (auto &mapping : mappings) {
      normalize_read_only_mapping(mapping);
    }

    std::scoped_lock lock { mutex_ };
    mappings_ = std::move(mappings);
  }

  std::vector<file_mapping::mapping_t>
  store_t::snapshot() const {
    std::scoped_lock lock { mutex_ };
    return mappings_;
  }

  nlohmann::json
  store_t::to_json() const {
    auto mappings = snapshot();
    nlohmann::json out = nlohmann::json::array();
    for (const auto &mapping : mappings) {
      out.push_back(mapping_to_config_json(mapping));
    }
    return out;
  }

  mutation_result_t
  store_t::add_quick_share(const fs::path &path) {
    std::error_code ec;
    if (path.empty() || !fs::is_directory(path, ec)) {
      return { false, {}, "path is not an existing directory" };
    }

    auto canonical = fs::weakly_canonical(path, ec);
    if (ec) {
      return { false, {}, ec.message() };
    }

    std::scoped_lock lock { mutex_ };
    auto existing = std::find_if(mappings_.begin(), mappings_.end(), [&](const file_mapping::mapping_t &mapping) {
      std::error_code mapping_ec;
      return fs::weakly_canonical(mapping.local_root, mapping_ec) == canonical && !mapping_ec;
    });
    if (existing != mappings_.end()) {
      return { true, *existing, {} };
    }

    file_mapping::mapping_t mapping;
    mapping.id = make_unique_id_locked(canonical);
    mapping.name = canonical.filename().empty() ? mapping.id : canonical.filename().generic_string();
    mapping.local_root = canonical;
    mapping.mode = file_mapping::access_mode_e::read;
    mapping.allow_delete = false;
    mapping.allow_execute = false;
    mapping.follow_reparse_points = false;
    mapping.max_file_size = 0;
    mapping.clients = {};

    mappings_.push_back(mapping);
    return { true, mapping, {} };
  }

  bool
  store_t::remove(const std::string &id) {
    std::scoped_lock lock { mutex_ };
    const auto old_size = mappings_.size();
    mappings_.erase(
      std::remove_if(mappings_.begin(), mappings_.end(), [&](const file_mapping::mapping_t &mapping) {
        return mapping.id == id;
      }),
      mappings_.end());
    return mappings_.size() != old_size;
  }

  mutation_result_t
  store_t::update(const std::string &id, const nlohmann::json &patch) {
    if (!patch.is_object()) {
      return { false, {}, "patch must be a JSON object" };
    }

    std::scoped_lock lock { mutex_ };
    auto it = std::find_if(mappings_.begin(), mappings_.end(), [&](const file_mapping::mapping_t &mapping) {
      return mapping.id == id;
    });
    if (it == mappings_.end()) {
      return { false, {}, "mapping was not found" };
    }

    auto updated = *it;
    if (patch.contains("name")) {
      if (!patch["name"].is_string() || patch["name"].get<std::string>().empty()) {
        return { false, {}, "name must be a non-empty string" };
      }
      updated.name = patch["name"].get<std::string>();
    }
    if (patch.contains("mode")) {
      if (!patch["mode"].is_string()) {
        return { false, {}, "mode must be a string" };
      }
      const auto mode = patch["mode"].get<std::string>();
      if (mode == "read") {
        updated.mode = file_mapping::access_mode_e::read;
      }
      else if (mode == "readwrite") {
        return { false, {}, "readwrite mode is not supported in the read-only phase" };
      }
      else {
        return { false, {}, "mode must be read or readwrite" };
      }
    }
    if (patch.contains("allow_delete")) {
      if (!patch["allow_delete"].is_boolean()) {
        return { false, {}, "allow_delete must be a boolean" };
      }
      if (patch["allow_delete"].get<bool>()) {
        return { false, {}, "allow_delete is not supported in the read-only phase" };
      }
      updated.allow_delete = patch["allow_delete"].get<bool>();
    }
    if (patch.contains("allow_execute")) {
      if (!patch["allow_execute"].is_boolean()) {
        return { false, {}, "allow_execute must be a boolean" };
      }
      if (patch["allow_execute"].get<bool>()) {
        return { false, {}, "allow_execute is not supported" };
      }
      updated.allow_execute = patch["allow_execute"].get<bool>();
    }
    if (patch.contains("follow_reparse_points")) {
      if (!patch["follow_reparse_points"].is_boolean()) {
        return { false, {}, "follow_reparse_points must be a boolean" };
      }
      if (patch["follow_reparse_points"].get<bool>()) {
        return { false, {}, "follow_reparse_points is not supported" };
      }
      updated.follow_reparse_points = patch["follow_reparse_points"].get<bool>();
    }
    if (patch.contains("max_file_size")) {
      std::uintmax_t max_file_size = 0;
      if (!read_uintmax_patch_value(patch["max_file_size"], max_file_size)) {
        return { false, {}, "max_file_size must be a non-negative integer" };
      }
      updated.max_file_size = max_file_size;
    }
    if (patch.contains("clients")) {
      if (!patch["clients"].is_array()) {
        return { false, {}, "clients must be an array" };
      }
      std::vector<std::string> clients;
      for (const auto &client : patch["clients"]) {
        if (!client.is_string()) {
          return { false, {}, "clients must contain only strings" };
        }
        clients.push_back(client.get<std::string>());
      }
      updated.clients = std::move(clients);
    }

    normalize_read_only_mapping(updated);
    *it = updated;
    return { true, updated, {} };
  }

  std::string
  store_t::make_unique_id_locked(const fs::path &path) const {
    const auto prefix = sanitize_id_prefix(path.filename().generic_string());
    auto id = prefix + "-" + short_hash(path);
    for (int suffix = 2; contains_id(mappings_, id); ++suffix) {
      id = prefix + "-" + short_hash(path) + "-" + std::to_string(suffix);
    }
    return id;
  }

  store_t &
  global() {
    static store_t store;
    return store;
  }

  nlohmann::json
  mapping_to_config_json(const file_mapping::mapping_t &mapping) {
    return {
      { "id", mapping.id },
      { "name", mapping.name },
      { "path", mapping.local_root.generic_string() },
      { "mode", mapping.mode == file_mapping::access_mode_e::read ? "read" : "readwrite" },
      { "allow_delete", mapping.allow_delete },
      { "allow_execute", mapping.allow_execute },
      { "follow_reparse_points", mapping.follow_reparse_points },
      { "max_file_size", mapping.max_file_size },
      { "clients", mapping.clients }
    };
  }

  std::string
  serialize_config_json(const std::vector<file_mapping::mapping_t> &mappings) {
    nlohmann::json root = nlohmann::json::array();
    for (const auto &mapping : mappings) {
      root.push_back(mapping_to_config_json(mapping));
    }
    return root.dump();
  }

  std::string
  serialize_config_value(const std::vector<file_mapping::mapping_t> &mappings) {
    return "base64:" + base64_encode(serialize_config_json(mappings));
  }

  bool
  persist_to_config(const store_t &store) {
    auto mappings = store.snapshot();
    const auto serialized = serialize_config_value(mappings);
    if (config::nvhttp.file_mappings == serialized) {
      return true;
    }
    if (!config::update_config({ { "file_mappings", serialized } }) && !config_file_has_file_mappings_value(serialized)) {
      return false;
    }
    config::nvhttp.file_mappings = serialized;
    return true;
  }
}  // namespace file_mapping_store
