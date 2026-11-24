/**
 * @file src/ai/proxy.h
 * @brief Transport-independent LLM proxy shared by Web UI, GameStream and ABR.
 */
#pragma once

#include <cstddef>
#include <functional>
#include <string>

namespace ai {
  // AI LLM Proxy — shared interface for nvhttp
  struct AiProxyResult {
    int httpCode;  // HTTP status code to return (200, 400, 401, 403, 429, 502, 500)
    std::string body;  // JSON response body
    std::string contentType;  // "application/json" or "text/event-stream"
  };

  /**
   * @brief Check if AI proxy is enabled and configured.
   */
  bool
  isAiEnabled();

  /**
   * @brief Process an AI chat completion request (non-streaming).
   * @param requestBody OpenAI-compatible JSON request body
   * @return AiProxyResult with status code and response body
   */
  AiProxyResult
  processAiChat(const std::string &requestBody);

  /**
   * @brief Process an AI chat completion request with streaming (SSE).
   * Calls chunkCallback for each SSE chunk received from upstream.
   * @param requestBody OpenAI-compatible JSON request body
   * @param chunkCallback Called with each data chunk; false aborts the upstream request.
   * @param isCancelled Optional cancellation poll, also called while upstream is idle.
   * Callback exceptions are caught before returning to libcurl.
   * @return AiProxyResult (httpCode=200 if streaming started, error otherwise)
   */
  AiProxyResult
  processAiChatStream(
    const std::string &requestBody,
    std::function<bool(const char *, size_t)> chunkCallback,
    std::function<bool()> isCancelled = {});
}  // namespace ai
