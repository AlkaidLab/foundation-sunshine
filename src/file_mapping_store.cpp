/**
 * @file src/file_mapping_store.cpp
 * @brief Thread-safe runtime store for host directory mappings.
 */
#include "file_mapping_store.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <sstream>
#include <system_error>

#include "config.h"
#include "file_handler.h"

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
  }  // namespace

  void
  store_t::replace(std::vector<file_mapping::mapping_t> mappings) {
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
        updated.mode = file_mapping::access_mode_e::readwrite;
      }
      else {
        return { false, {}, "mode must be read or readwrite" };
      }
    }
    if (patch.contains("allow_delete")) {
      if (!patch["allow_delete"].is_boolean()) {
        return { false, {}, "allow_delete must be a boolean" };
      }
      updated.allow_delete = patch["allow_delete"].get<bool>();
    }
    if (patch.contains("allow_execute")) {
      if (!patch["allow_execute"].is_boolean()) {
        return { false, {}, "allow_execute must be a boolean" };
      }
      updated.allow_execute = patch["allow_execute"].get<bool>();
    }
    if (patch.contains("follow_reparse_points")) {
      if (!patch["follow_reparse_points"].is_boolean()) {
        return { false, {}, "follow_reparse_points must be a boolean" };
      }
      updated.follow_reparse_points = patch["follow_reparse_points"].get<bool>();
    }
    if (patch.contains("max_file_size")) {
      if (!patch["max_file_size"].is_number_unsigned()) {
        return { false, {}, "max_file_size must be an unsigned integer" };
      }
      updated.max_file_size = patch["max_file_size"].get<std::uintmax_t>();
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

    if (updated.mode == file_mapping::access_mode_e::read && updated.allow_delete) {
      if (patch.contains("mode") && patch["mode"].get<std::string>() == "read" && !patch.contains("allow_delete")) {
        updated.allow_delete = false;
      }
      else {
        return { false, {}, "allow_delete requires readwrite mode" };
      }
    }

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

  bool
  persist_to_config(const store_t &store) {
    auto mappings = store.snapshot();
    const auto serialized = serialize_config_json(mappings);
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
