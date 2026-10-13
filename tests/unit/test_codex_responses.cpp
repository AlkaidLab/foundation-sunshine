/**
 * @file tests/unit/test_codex_responses.cpp
 * @brief Offline tests for the Codex Responses wire format.
 */

#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "src/ai/codex_responses.h"

namespace {
  using nlohmann::json;
}

TEST(CodexResponsesTest, ConvertsInstructionsHistoryAndDataImage) {
  const json chat = {
    { "model", "gpt-5-codex" },
    { "temperature", 0.3 },
    { "max_tokens", 2048 },
    { "messages", json::array({
                    { { "role", "system" }, { "content", "Be concise." } },
                    { { "role", "user" }, { "content", "First question" } },
                    { { "role", "assistant" }, { "content", "First answer" } },
                    { { "role", "user" }, { "content", json::array({
                                                { { "type", "text" }, { "text", "Describe this" } },
                                                { { "type", "image_url" }, { "image_url", {
                                                                                 { "url", "data:image/png;base64,aGVsbG8=" },
                                                                                 { "detail", "low" },
                                                                               } } },
                                              }) } },
                  }) },
  };
  const auto converted = codex_responses::make_request(chat.dump());
  ASSERT_TRUE(converted.success) << converted.error;
  EXPECT_EQ(converted.model, "gpt-5-codex");
  const auto body = json::parse(converted.body);
  EXPECT_EQ(body["model"], "gpt-5-codex");
  EXPECT_EQ(body["instructions"], "Be concise.");
  EXPECT_EQ(body["store"], false);
  EXPECT_EQ(body["stream"], true);
  EXPECT_EQ(body["text"]["verbosity"], "low");
  EXPECT_EQ(body["include"], json::array({ "reasoning.encrypted_content" }));
  EXPECT_EQ(body["tool_choice"], "auto");
  EXPECT_EQ(body["parallel_tool_calls"], true);
  EXPECT_FALSE(body.contains("temperature"));
  EXPECT_FALSE(body.contains("max_tokens"));
  ASSERT_EQ(body["input"].size(), 3);
  EXPECT_EQ(body["input"][0]["role"], "user");
  EXPECT_EQ(body["input"][0]["content"][0]["type"], "input_text");
  EXPECT_EQ(body["input"][1]["role"], "assistant");
  EXPECT_EQ(body["input"][1]["type"], "message");
  EXPECT_EQ(body["input"][1]["status"], "completed");
  EXPECT_EQ(body["input"][1]["id"], "msg_sunshine_2");
  EXPECT_EQ(body["input"][1]["content"][0]["type"], "output_text");
  EXPECT_EQ(body["input"][1]["content"][0]["text"], "First answer");
  EXPECT_EQ(body["input"][1]["content"][0]["annotations"], json::array());
  EXPECT_EQ(body["input"][2]["content"][1]["type"], "input_image");
  EXPECT_EQ(body["input"][2]["content"][1]["image_url"], "data:image/png;base64,aGVsbG8=");
  EXPECT_EQ(body["input"][2]["content"][1]["detail"], "low");
}

TEST(CodexResponsesTest, RejectsMalformedAndUnsupportedInput) {
  const auto malformed = codex_responses::make_request("{");
  EXPECT_FALSE(malformed.success);
  EXPECT_NE(malformed.error.find("JSON"), std::string::npos);

  const auto no_model = codex_responses::make_request(R"({"messages":[{"role":"user","content":"Hi"}]})");
  EXPECT_FALSE(no_model.success);
  EXPECT_NE(no_model.error.find("model"), std::string::npos);

  const auto tool = codex_responses::make_request(R"({"model":"gpt-5-codex","messages":[{"role":"tool","content":"x"}]})");
  EXPECT_FALSE(tool.success);
  EXPECT_NE(tool.error.find("role"), std::string::npos);

  const auto tool_call = codex_responses::make_request(R"({"model":"gpt-5-codex","messages":[{"role":"assistant","content":"x","tool_calls":[]}]})");
  EXPECT_FALSE(tool_call.success);
  EXPECT_NE(tool_call.error.find("tool_calls"), std::string::npos);

  const auto image = codex_responses::make_request(R"({"model":"gpt-5-codex","messages":[{"role":"user","content":[{"type":"image_url","image_url":{"url":"https://example.com/image.png"}}]}]})");
  EXPECT_FALSE(image.success);
  EXPECT_NE(image.error.find("data URI"), std::string::npos);

  const auto late_system = codex_responses::make_request(R"({"model":"gpt-5-codex","messages":[{"role":"user","content":"Hi"},{"role":"system","content":"late"}]})");
  EXPECT_FALSE(late_system.success);
  EXPECT_NE(late_system.error.find("precede"), std::string::npos);

  const auto no_system = codex_responses::make_request(R"({"model":"gpt-5-codex","messages":[{"role":"user","content":"Hi"}]})");
  ASSERT_TRUE(no_system.success) << no_system.error;
  EXPECT_EQ(json::parse(no_system.body)["instructions"], "You are a helpful assistant.");
}

TEST(CodexResponsesTest, SelectsAccountModeFallbackModel) {
  EXPECT_EQ(codex_responses::configured_model_or_default("gpt-6-sol"), "gpt-6-sol");
  EXPECT_EQ(codex_responses::configured_model_or_default("gpt-4.1-mini"), "gpt-6-luna");
  EXPECT_EQ(codex_responses::configured_model_or_default(""), "gpt-6-luna");
}

TEST(CodexResponsesTest, PrefersSpecificStreamErrorsOverCurlWriteFailure) {
  EXPECT_EQ(codex_responses::stream_error("callback failed", "quota exceeded", false), "callback failed");
  EXPECT_EQ(codex_responses::stream_error("", "quota exceeded", false), "quota exceeded");
  EXPECT_EQ(codex_responses::stream_error("", "", false), "ChatGPT Codex stream was interrupted");
  EXPECT_TRUE(codex_responses::stream_error("", "", true).empty());
}

TEST(CodexResponsesTest, DecodesSseAcrossArbitraryChunkBoundaries) {
  const std::string stream =
    ": keepalive\r\n\r\n"
    "event: response.output_text.delta\r\n"
    "data: {\"delta\":\"Hel\"}\r\n\r\n"
    "data: {\"type\":\"response.output_text.delta\",\"delta\":\"lo\"}\n\n"
    "data: {\"type\":\"response.completed\",\"response\":{\"status\":\"completed\"}}\n\n"
    "data: [DONE]\n\n";
  codex_responses::sse_decoder_t decoder;
  std::vector<std::string> deltas;
  for (const char ch : stream) {
    const std::string byte(1, ch);
    decoder.feed(byte, [&](const std::string &delta) { deltas.push_back(delta); });
  }
  decoder.finish();
  EXPECT_TRUE(decoder.completed()) << decoder.error();
  EXPECT_TRUE(decoder.error().empty());
  EXPECT_EQ(decoder.text(), "Hello");
  EXPECT_EQ(deltas, (std::vector<std::string> { "Hel", "lo" }));
}

TEST(CodexResponsesTest, HandlesFinalTextAndFailureEvents) {
  codex_responses::sse_decoder_t final_only;
  final_only.feed(R"(data: {"type":"response.completed","response":{"output":[{"type":"message","content":[{"type":"output_text","text":"answer"}]}]}}

)");
  final_only.finish();
  EXPECT_TRUE(final_only.completed());
  EXPECT_EQ(final_only.text(), "answer");

  codex_responses::sse_decoder_t failed;
  failed.feed("event: response.failed\ndata: {\"response\":{\"error\":{\"message\":\"quota exceeded\"}}}\n\n");
  failed.finish();
  EXPECT_FALSE(failed.completed());
  EXPECT_EQ(failed.error(), "quota exceeded");

  codex_responses::sse_decoder_t incomplete;
  incomplete.feed("data: {\"type\":\"response.incomplete\",\"response\":{\"incomplete_details\":{\"reason\":\"max_output_tokens\"}}}\n\n");
  EXPECT_NE(incomplete.error().find("max_output_tokens"), std::string::npos);
}

TEST(CodexResponsesTest, StreamsRefusalWithoutRepeatingFinalText) {
  codex_responses::sse_decoder_t decoder;
  std::vector<std::string> deltas;
  const auto on_delta = [&](const std::string &delta) { deltas.push_back(delta); };
  decoder.feed("data: {\"type\":\"response.refusal.delta\",\"delta\":\"Cannot \"}\n\n", on_delta);
  decoder.feed("data: {\"type\":\"response.refusal.delta\",\"delta\":\"help\"}\n\n", on_delta);
  decoder.feed("data: {\"type\":\"response.refusal.done\",\"refusal\":\"Cannot help\"}\n\n", on_delta);
  decoder.feed("data: {\"type\":\"response.completed\",\"response\":{\"output\":[{\"type\":\"message\",\"content\":[{\"type\":\"refusal\",\"refusal\":\"Cannot help\"}]}]}}\n\n", on_delta);
  decoder.finish();
  EXPECT_TRUE(decoder.completed()) << decoder.error();
  EXPECT_EQ(decoder.text(), "Cannot help");
  EXPECT_EQ(deltas, (std::vector<std::string> { "Cannot ", "help" }));
}

TEST(CodexResponsesTest, UsesFinalRefusalWhenNoDeltasArrive) {
  codex_responses::sse_decoder_t decoder;
  std::vector<std::string> deltas;
  decoder.feed("data: {\"type\":\"response.completed\",\"response\":{\"output\":[{\"type\":\"message\",\"content\":[{\"type\":\"refusal\",\"refusal\":\"Request refused\"}]}]}}\n\n",
               [&](const std::string &delta) { deltas.push_back(delta); });
  decoder.finish();
  EXPECT_TRUE(decoder.completed()) << decoder.error();
  EXPECT_EQ(decoder.text(), "Request refused");
  EXPECT_EQ(deltas, (std::vector<std::string> { "Request refused" }));
}

TEST(CodexResponsesTest, RejectsMalformedOrTruncatedStreamsAndLargeEvents) {
  codex_responses::sse_decoder_t malformed;
  malformed.feed("data: {bad}\n\n");
  EXPECT_NE(malformed.error().find("Malformed"), std::string::npos);

  codex_responses::sse_decoder_t truncated;
  truncated.feed("data: {\"type\":\"response.output_text.delta\",\"delta\":\"partial\"}\n\n");
  truncated.finish();
  EXPECT_EQ(truncated.text(), "partial");
  EXPECT_FALSE(truncated.completed());
  EXPECT_NE(truncated.error().find("before response.completed"), std::string::npos);

  codex_responses::sse_decoder_t oversized;
  oversized.feed("data: " + std::string(1024 * 1024, 'x'));
  EXPECT_NE(oversized.error().find("1 MiB"), std::string::npos);
}
