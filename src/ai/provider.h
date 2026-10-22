/**
 * @file src/ai/provider.h
 * @brief Provider compatibility helpers shared by AI proxy and model discovery.
 */
#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace ai {
  bool
  isAnthropicProvider(const nlohmann::json &cfg);
  bool
  isApiKeyRequired(const nlohmann::json &cfg);
  bool
  hasSuffix(const std::string &value, const std::string &suffix);
  std::string
  buildAiEndpoint(std::string apiBase, bool isAnthropic);
  std::string
  convertToAnthropicFormat(const std::string &openaiBody, const std::string &model);
  std::string
  convertFromAnthropicFormat(const std::string &anthropicResponse);
}  // namespace ai
