/**
 * @file src/ai/provider.cpp
 * @brief LLM endpoint and request/response compatibility.
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

#include "provider.h"

using namespace std::literals;
using json = nlohmann::json;

namespace ai {

  /**
   * @brief 检测 provider 是否为 Anthropic（需要不同的 API 格式）
   */
  bool
  isAnthropicProvider(const nlohmann::json &cfg) {
    std::string compatibility = cfg.value("compatibility", "");
    std::string provider = cfg.value("provider", "");
    std::string apiBase = cfg.value("apiBase", "");
    return compatibility == "anthropic-messages" || provider == "anthropic" || apiBase.find("anthropic.com") != std::string::npos;
  }

  bool
  isApiKeyRequired(const nlohmann::json &cfg) {
    std::string provider = cfg.value("provider", "");
    std::string apiBase = cfg.value("apiBase", "");
    return provider != "ollama" &&
           apiBase.find("localhost") == std::string::npos &&
           apiBase.find("127.0.0.1") == std::string::npos &&
           apiBase.find("[::1]") == std::string::npos;
  }

  bool
  hasSuffix(const std::string &value, const std::string &suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
  }

  std::string
  buildAiEndpoint(std::string apiBase, bool isAnthropic) {
    while (!apiBase.empty() && apiBase.back() == '/') {
      apiBase.pop_back();
    }

    if (isAnthropic) {
      if (hasSuffix(apiBase, "/v1/messages") || hasSuffix(apiBase, "/messages")) {
        return apiBase;
      }
      if (hasSuffix(apiBase, "/v1")) {
        return apiBase + "/messages";
      }
      return apiBase + "/v1/messages";
    }

    if (hasSuffix(apiBase, "/chat/completions")) {
      return apiBase;
    }
    return apiBase + "/chat/completions";
  }

  /**
   * @brief 将 OpenAI 格式的请求转换为 Anthropic 格式
   */
  std::string
  convertToAnthropicFormat(const std::string &openaiBody, const std::string &model) {
    try {
      auto input = nlohmann::json::parse(openaiBody);
      nlohmann::json anthropic;

      anthropic["model"] = input.value("model", model);
      anthropic["max_tokens"] = input.value("max_tokens", 4096);

      // 提取 system message 和 user/assistant messages
      if (input.contains("messages")) {
        nlohmann::json messages = nlohmann::json::array();
        for (auto &msg : input["messages"]) {
          std::string role = msg.value("role", "");
          if (role == "system") {
            anthropic["system"] = msg.value("content", "");
          }
          else {
            messages.push_back(msg);
          }
        }
        anthropic["messages"] = messages;
      }

      // 转换 temperature, top_p 等通用参数
      if (input.contains("temperature")) anthropic["temperature"] = input["temperature"];
      if (input.contains("top_p")) anthropic["top_p"] = input["top_p"];
      if (input.contains("stream")) anthropic["stream"] = input["stream"];

      return anthropic.dump();
    }
    catch (...) {
      return openaiBody;  // 转换失败，原样返回
    }
  }

  /**
   * @brief 将 Anthropic 格式的响应转换回 OpenAI 格式
   */
  std::string
  convertFromAnthropicFormat(const std::string &anthropicResponse) {
    try {
      auto resp = nlohmann::json::parse(anthropicResponse);
      nlohmann::json openai;

      openai["id"] = resp.value("id", "");
      openai["object"] = "chat.completion";
      openai["model"] = resp.value("model", "");

      // 转换 content blocks
      nlohmann::json choice;
      choice["index"] = 0;
      choice["finish_reason"] = resp.value("stop_reason", "stop");

      std::string content;
      if (resp.contains("content") && resp["content"].is_array()) {
        for (auto &block : resp["content"]) {
          if (block.value("type", "") == "text") {
            content += block.value("text", "");
          }
        }
      }
      choice["message"]["role"] = "assistant";
      choice["message"]["content"] = content;
      openai["choices"] = nlohmann::json::array({ choice });

      // 转换 usage
      if (resp.contains("usage")) {
        openai["usage"]["prompt_tokens"] = resp["usage"].value("input_tokens", 0);
        openai["usage"]["completion_tokens"] = resp["usage"].value("output_tokens", 0);
        openai["usage"]["total_tokens"] =
          resp["usage"].value("input_tokens", 0) + resp["usage"].value("output_tokens", 0);
      }

      return openai.dump();
    }
    catch (...) {
      return anthropicResponse;
    }
  }

}  // namespace ai
