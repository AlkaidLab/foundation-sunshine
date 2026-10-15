/**
 * @file tests/unit/test_ai_provider.cpp
 * @brief Regression coverage for the shared LLM compatibility service.
 */
#include "src/ai/provider.h"
#include <gtest/gtest.h>

TEST(AiProvider, NormalizesEndpointSuffixes) {
  EXPECT_EQ(ai::buildAiEndpoint("https://example.test/v1///", false), "https://example.test/v1/chat/completions");
  EXPECT_EQ(ai::buildAiEndpoint("https://example.test/v1/chat/completions/", false), "https://example.test/v1/chat/completions");
  EXPECT_EQ(ai::buildAiEndpoint("https://example.test", true), "https://example.test/v1/messages");
  EXPECT_EQ(ai::buildAiEndpoint("https://example.test/v1/", true), "https://example.test/v1/messages");
  EXPECT_EQ(ai::buildAiEndpoint("https://example.test/messages", true), "https://example.test/messages");
}

TEST(AiProvider, PreservesProviderAndLocalCredentialRules) {
  EXPECT_TRUE(ai::isAnthropicProvider({ { "compatibility", "anthropic-messages" } }));
  EXPECT_TRUE(ai::isAnthropicProvider({ { "provider", "anthropic" } }));
  EXPECT_TRUE(ai::isAnthropicProvider({ { "apiBase", "https://api.anthropic.com" } }));
  EXPECT_FALSE(ai::isAnthropicProvider({ { "provider", "openai" } }));
  EXPECT_FALSE(ai::isApiKeyRequired({ { "provider", "ollama" } }));
  for (const auto *url : { "http://localhost:11434", "http://127.0.0.1:11434", "http://[::1]:11434" }) {
    EXPECT_FALSE(ai::isApiKeyRequired({ { "apiBase", url } }));
  }
  EXPECT_TRUE(ai::isApiKeyRequired({ { "apiBase", "https://example.test/v1" } }));
}

TEST(AiProvider, ConvertsChatRequestWithoutLosingParameters) {
  const nlohmann::json input {
    { "messages", { { { "role", "system" }, { "content", "instructions" } }, { { "role", "user" }, { "content", "hello" } } } },
    { "temperature", 0.4 },
    { "top_p", 0.8 },
    { "stream", false },
    { "max_tokens", 512 },
  };
  const auto output = nlohmann::json::parse(ai::convertToAnthropicFormat(input.dump(), "default-model"));
  EXPECT_EQ(output["system"], "instructions");
  EXPECT_EQ(output["messages"].size(), 1);
  EXPECT_EQ(output["messages"][0], input["messages"][1]);
  EXPECT_EQ(output["model"], "default-model");
  EXPECT_EQ(output["max_tokens"], 512);
  EXPECT_EQ(output["temperature"], input["temperature"]);
  EXPECT_EQ(output["top_p"], input["top_p"]);
  EXPECT_EQ(output["stream"], false);
}

TEST(AiProvider, ConvertsResponseContentAndUsage) {
  const nlohmann::json input {
    { "id", "message-1" },
    { "model", "test-model" },
    { "stop_reason", "end_turn" },
    { "content", { { { "type", "text" }, { "text", "hello " } }, { { "type", "tool_use" } }, { { "type", "text" }, { "text", "world" } } } },
    { "usage", { { "input_tokens", 11 }, { "output_tokens", 7 } } },
  };
  const auto output = nlohmann::json::parse(ai::convertFromAnthropicFormat(input.dump()));
  EXPECT_EQ(output["choices"][0]["message"]["content"], "hello world");
  EXPECT_EQ(output["choices"][0]["finish_reason"], "end_turn");
  EXPECT_EQ(output["usage"]["total_tokens"], 18);
  EXPECT_EQ(output["object"], "chat.completion");
}

TEST(AiProvider, PreservesMalformedPayloadFallback) {
  const std::string malformed = "{not-json";
  EXPECT_EQ(ai::convertToAnthropicFormat(malformed, "model"), malformed);
  EXPECT_EQ(ai::convertFromAnthropicFormat(malformed), malformed);
}
