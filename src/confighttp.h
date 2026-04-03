/**
 * @file src/confighttp.h
 * @brief Declarations for the Web UI Config HTTP server.
 */
#pragma once

#include <functional>
#include <string>

#include "thread_safe.h"

#define WEB_DIR SUNSHINE_ASSETS_DIR "/web/"

namespace confighttp {
  constexpr auto PORT_HTTPS = 1;
  void
  start();

  bool
  saveVddSettings(std::string resArray, std::string fpsArray, std::string gpu_name);

  // AI LLM Proxy — shared interface for nvhttp
  struct AiProxyResult {
    int httpCode;  // HTTP status code to return (200, 400, 403, 502, 500)
    std::string body;  // JSON response body
    std::string contentType;  // "application/json" or "text/event-stream"
  };

  /**
   * @brief Check if AI proxy is enabled and configured.
   */
  bool isAiEnabled();

  /**
   * @brief Process an AI chat completion request (non-streaming).
   * @param requestBody OpenAI-compatible JSON request body
   * @return AiProxyResult with status code and response body
   */
  AiProxyResult processAiChat(const std::string &requestBody);

  /**
   * @brief Process an AI chat completion request with streaming (SSE).
   * Calls chunkCallback for each SSE chunk received from upstream.
   * @param requestBody OpenAI-compatible JSON request body
   * @param chunkCallback Called with each data chunk from upstream
   * @return AiProxyResult (httpCode=200 if streaming started, error otherwise)
   */
  AiProxyResult processAiChatStream(
    const std::string &requestBody,
    std::function<void(const char *, size_t)> chunkCallback);
}  // namespace confighttp

// mime types map
const std::map<std::string, std::string> mime_types = {
  { "css", "text/css" },
  { "gif", "image/gif" },
  { "htm", "text/html" },
  { "html", "text/html" },
  { "ico", "image/x-icon" },
  { "jpeg", "image/jpeg" },
  { "jpg", "image/jpeg" },
  { "js", "application/javascript" },
  { "json", "application/json" },
  { "png", "image/png" },
  { "svg", "image/svg+xml" },
  { "ttf", "font/ttf" },
  { "txt", "text/plain" },
  { "woff2", "font/woff2" },
  { "xml", "text/xml" },
};
