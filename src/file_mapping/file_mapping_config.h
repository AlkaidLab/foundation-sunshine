/**
 * @file src/file_mapping_config.h
 * @brief Parse file mapping configuration into runtime mappings.
 */
#pragma once

#include <string>
#include <vector>

#include "file_mapping.h"

namespace file_mapping_config {
  struct parse_result_t {
    std::vector<file_mapping::mapping_t> mappings;
    std::vector<std::string> warnings;
  };

  parse_result_t
  parse_mappings_json(const std::string &json_text);
}  // namespace file_mapping_config
