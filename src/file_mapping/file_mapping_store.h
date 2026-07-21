/**
 * @file src/file_mapping_store.h
 * @brief Thread-safe runtime store for host directory mappings.
 */
#pragma once

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "file_mapping.h"

namespace file_mapping_store {
  struct mutation_result_t {
    bool ok = false;
    file_mapping::mapping_t mapping;
    std::string error;
  };

  class store_t {
  public:
    void
    replace(std::vector<file_mapping::mapping_t> mappings);

    std::vector<file_mapping::mapping_t>
    snapshot() const;

    nlohmann::json
    to_json() const;

    mutation_result_t
    add_quick_share(const std::filesystem::path &path);

    bool
    remove(const std::string &id);

    mutation_result_t
    update(const std::string &id, const nlohmann::json &patch);

  private:
    std::string
    make_unique_id_locked(const std::filesystem::path &path) const;

    mutable std::mutex mutex_;
    std::vector<file_mapping::mapping_t> mappings_;
  };

  store_t &
  global();

  nlohmann::json
  mapping_to_config_json(const file_mapping::mapping_t &mapping);

  std::string
  serialize_config_json(const std::vector<file_mapping::mapping_t> &mappings);

  std::string
  serialize_config_value(const std::vector<file_mapping::mapping_t> &mappings);

  bool
  persist_to_config(const store_t &store);
}  // namespace file_mapping_store
