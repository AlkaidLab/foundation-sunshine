/**
 * @file src/confighttp.h
 * @brief Declarations for the Web UI Config HTTP server.
 */
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "thread_safe.h"

#define WEB_DIR SUNSHINE_ASSETS_DIR "/web/"

namespace confighttp {
  constexpr auto PORT_HTTPS = 1;
  struct VddModeCombinationLimit {
    std::size_t refresh_rate_count;
    std::size_t max_combination_count;
  };

  struct VddTemporaryModeLimit {
    std::size_t global_resolution_count;
    std::size_t global_refresh_rate_count;
    std::size_t max_temporary_mode_count;
  };

  // Empirically stable Windows/IddCx mode-list budgets for VDD XML reloads.
  // Measured with ZakoVDD disable/enable + CREATEMONITOR probes on 2026-06-05.
  // Effective mode count is unique resolutions * global refresh rates.
  inline constexpr VddModeCombinationLimit VDD_MODE_COMBINATION_LIMITS[] = {
    { 1, 342 },  // 342 accepted, 343 failed.
    { 2, 134 },  // 134 accepted in targeted create probe.
    { 3, 69 },  // 69 accepted, 72 failed in the longer-wait probe.
    { 4, 72 },  // 72 accepted, 76 failed.
    { 5, 65 },  // 65 accepted in targeted create probe.
    { 6, 66 },  // 66 accepted; 72 was intermittent, 78 failed.
    { 7, 70 },  // 70 accepted in targeted create probe.
    { 8, 80 },  // 80 accepted, 104 failed.
    { 9, 72 },  // 72 accepted in targeted create probe.
    { 10, 80 },  // 80 accepted, 90 failed.
  };

  inline constexpr std::size_t VDD_FALLBACK_MODE_COMBINATION_LIMIT = 66;

  // Per-resolution refresh_rate nodes do not behave like a pure global Cartesian product.
  // These are max temporary exact modes, measured with CREATEMONITOR probes.
  inline constexpr VddTemporaryModeLimit VDD_TEMPORARY_MODE_LIMITS[] = {
    { 0, 1, 171 },  // 171 accepted, 172 failed when every node had refresh_rate and one global rate existed.
    { 0, 6, 43 },  // 43 accepted, 44 failed when every node had refresh_rate and six global rates existed.
    { 8, 6, 27 },  // Original 8 global resolutions: 27 temporary exact modes accepted, 28 failed.
  };

  inline constexpr std::size_t VDD_FALLBACK_TEMPORARY_MODE_LIMIT = 0;

  // 临时模式缓存上限：最多保留最近使用的5个会话模式。
  inline constexpr std::size_t VDD_MAX_CACHED_TEMPORARY_MODES = 5;

  inline constexpr std::size_t
  vdd_max_mode_combination_count(std::size_t refresh_rate_count) {
    if (refresh_rate_count == 0) {
      return 0;
    }

    for (const auto &limit : VDD_MODE_COMBINATION_LIMITS) {
      if (refresh_rate_count == limit.refresh_rate_count) {
        return limit.max_combination_count;
      }
    }

    for (const auto &limit : VDD_MODE_COMBINATION_LIMITS) {
      if (refresh_rate_count <= limit.refresh_rate_count) {
        return limit.max_combination_count;
      }
    }

    return VDD_FALLBACK_MODE_COMBINATION_LIMIT;
  }

  inline constexpr std::size_t
  vdd_max_temporary_mode_count(std::size_t global_resolution_count, std::size_t global_refresh_rate_count) {
    if (global_refresh_rate_count == 0) {
      return 0;
    }

    for (const auto &limit : VDD_TEMPORARY_MODE_LIMITS) {
      if (global_resolution_count == limit.global_resolution_count &&
          global_refresh_rate_count == limit.global_refresh_rate_count) {
        return limit.max_temporary_mode_count;
      }
    }

    const auto global_mode_budget = vdd_max_mode_combination_count(global_refresh_rate_count);
    const auto global_mode_count = global_resolution_count * global_refresh_rate_count;
    return global_mode_count < global_mode_budget ? global_mode_budget - global_mode_count : VDD_FALLBACK_TEMPORARY_MODE_LIMIT;
  }

  void
  start();

  // Pair of "WIDTHxHEIGHT" and refresh-rate text, e.g. {"1920x1080", "60"}.
  using VddMode = std::pair<std::string, std::string>;

  bool
  saveVddSettings(std::string resArray, std::string fpsArray, std::string gpu_name);

  bool
  saveVddModeSettings(const std::vector<VddMode> &modes, std::string gpu_name);

  bool
  saveVddModeSettings(const std::vector<VddMode> &global_modes, const std::vector<VddMode> &temporary_modes, std::string gpu_name);

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
