/**
 * @file src/file_mapping_operations.h
 * @brief Read-only file mapping RPC execution helpers.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "file_mapping.h"
#include "file_mapping_rpc.h"

namespace file_mapping::operations {
  struct execution_context_t {
    std::vector<mapping_t> mappings;
    std::function<std::vector<mapping_t>()> mapping_provider;
    std::string peer_uuid;
    std::uint32_t max_read_bytes = 1024 * 1024;
    std::uint32_t max_list_entries = 4096;
  };

  nlohmann::json
  mapping_to_json(const mapping_t &mapping);

  nlohmann::json
  execute_control_message(const rpc::parse_result_t &message, const execution_context_t &context);
}  // namespace file_mapping::operations
