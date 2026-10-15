/**
 * @file src/ai/config_store.h
 * @brief Serialized AI configuration and secure credential persistence.
 */
#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace ai {
  nlohmann::json
  loadAiConfig();
  // Parse and apply a partial update under the configuration store's transaction lock.
  nlohmann::json
  save_config(const std::string &request_body);
}  // namespace ai
