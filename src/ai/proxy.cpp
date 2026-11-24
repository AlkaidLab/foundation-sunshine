/**
 * @file src/ai/proxy.cpp
 * @brief Shared LLM proxy execution and streaming.
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

#include "proxy.h"
#include "config_store.h"
#include "provider.h"
#include "src/httpcommon.h"
#include "src/logging.h"
#include <curl/curl.h>
#include <map>

using namespace std::literals;
using json = nlohmann::json;

namespace ai {

  namespace {
    struct StreamCallbackContext {
      CURL *curl = nullptr;
      std::function<bool(const char *, size_t)> callback;
      std::function<bool()> is_cancelled;
      std::string error_body;
    };

  }  // namespace

  bool
  isAiEnabled() {
    auto cfg = loadAiConfig();
    return cfg.value("enabled", false) &&
           (!isApiKeyRequired(cfg) || !cfg.value("apiKey", "").empty()) &&
           !cfg.value("apiBase", "").empty();
  }

  /**
   * @brief Prepare AI proxy request: validate config, build URL, headers, convert body.
   * @return empty targetUrl on error (result is filled with error info)
   */
  static bool
  prepareAiRequest(
    const std::string &requestBody,
    std::string &targetUrl,
    std::string &processedBody,
    std::map<std::string, std::string> &proxyHeaders,
    bool &isAnthropic,
    bool &isStream,
    AiProxyResult &result) {
    auto cfg = loadAiConfig();

    if (!cfg.value("enabled", false)) {
      result = { 403, R"({"error":{"message":"AI proxy is not enabled","type":"invalid_request_error"}})", "application/json" };
      return false;
    }

    std::string apiBase = cfg.value("apiBase", "");
    std::string apiKey = cfg.value("apiKey", "");
    std::string defaultModel = cfg.value("model", "");

    if (apiBase.empty() || (apiKey.empty() && isApiKeyRequired(cfg))) {
      result = { 400, R"({"error":{"message":"AI proxy not configured: missing apiBase or apiKey","type":"invalid_request_error"}})", "application/json" };
      return false;
    }

    if (requestBody.empty()) {
      result = { 400, R"({"error":{"message":"Empty request body","type":"invalid_request_error"}})", "application/json" };
      return false;
    }

    processedBody = requestBody;
    isStream = false;

    try {
      auto reqJson = nlohmann::json::parse(processedBody);
      isStream = reqJson.value("stream", false);
      if (!reqJson.contains("model") && !defaultModel.empty()) {
        reqJson["model"] = defaultModel;
        processedBody = reqJson.dump();
      }
    }
    catch (...) {
    }

    isAnthropic = isAnthropicProvider(cfg);

    targetUrl = buildAiEndpoint(apiBase, isAnthropic);

    if (isAnthropic) {
      // Anthropic 流式 SSE 格式与 OpenAI 不兼容，强制走非流式以保证响应格式一致
      if (isStream) {
        try {
          auto reqJson = nlohmann::json::parse(processedBody);
          reqJson["stream"] = false;
          processedBody = reqJson.dump();
        }
        catch (...) {
        }
        isStream = false;
      }
      processedBody = convertToAnthropicFormat(processedBody, defaultModel);
      if (!apiKey.empty()) {
        proxyHeaders["x-api-key"] = apiKey;
      }
      proxyHeaders["anthropic-version"] = "2023-06-01";
    }
    else {
      if (!apiKey.empty()) {
        proxyHeaders["Authorization"] = "Bearer " + apiKey;
      }
    }

    BOOST_LOG(info) << "AI proxy forwarding to: " << targetUrl << (isStream ? " (stream)" : "");
    return true;
  }

  AiProxyResult
  processAiChat(const std::string &requestBody) {
    std::string targetUrl, processedBody;
    std::map<std::string, std::string> proxyHeaders;
    bool isAnthropic = false, isStream = false;
    AiProxyResult result;

    if (!prepareAiRequest(requestBody, targetUrl, processedBody, proxyHeaders, isAnthropic, isStream, result)) {
      return result;
    }

    std::string responseBody;
    long httpCode = 0;

    try {
      bool ok = ::http::post_json(targetUrl, processedBody, proxyHeaders, responseBody, httpCode, 120);
      if (ok) {
        if (isAnthropic && httpCode >= 200 && httpCode < 300) {
          responseBody = convertFromAnthropicFormat(responseBody);
        }
        int statusCode = (httpCode >= 200 && httpCode < 300) ? 200 : 502;
        return { statusCode, responseBody, "application/json" };
      }
      else {
        return { 502, R"({"error":{"message":"Failed to connect to upstream LLM API","type":"upstream_error"}})", "application/json" };
      }
    }
    catch (const std::exception &e) {
      BOOST_LOG(error) << "AI proxy exception: " << e.what();
      nlohmann::json err;
      err["error"]["message"] = std::string("AI proxy exception: ") + e.what();
      err["error"]["type"] = "internal_error";
      return { 500, err.dump(), "application/json" };
    }
  }

  static size_t
  stream_func_callback(char *ptr, size_t size, size_t nmemb, void *userdata) noexcept {
    size_t realsize = size * nmemb;
    auto *ctx = static_cast<StreamCallbackContext *>(userdata);
    try {
      long http_code = 0;
      if (curl_easy_getinfo(ctx->curl, CURLINFO_RESPONSE_CODE, &http_code) != CURLE_OK) return 0;
      if (http_code < 200 || http_code >= 300) {
        // Do not let an error payload trigger a downstream 200/SSE header.
        // Keep buffering bounded even if a failing upstream sends a large body.
        constexpr size_t max_error_body = 1024 * 1024;
        if (realsize > max_error_body - ctx->error_body.size()) return 0;
        ctx->error_body.append(ptr, realsize);
        return realsize;
      }
      return ctx->callback(ptr, realsize) ? realsize : 0;
    }
    catch (...) {
      return 0;
    }
  }

  static int
  stream_progress_callback(void *userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t) noexcept {
    auto *ctx = static_cast<StreamCallbackContext *>(userdata);
    try {
      return ctx->is_cancelled && ctx->is_cancelled() ? 1 : 0;
    }
    catch (...) {
      return 1;
    }
  }

  AiProxyResult
  processAiChatStream(
    const std::string &requestBody,
    std::function<bool(const char *, size_t)> chunkCallback,
    std::function<bool()> isCancelled) {
    std::string targetUrl, processedBody;
    std::map<std::string, std::string> proxyHeaders;
    bool isAnthropic = false, isStream = false;
    AiProxyResult result;

    if (!prepareAiRequest(requestBody, targetUrl, processedBody, proxyHeaders, isAnthropic, isStream, result)) {
      return result;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
      return { 500, R"({"error":{"message":"Failed to initialize CURL","type":"internal_error"}})", "application/json" };
    }

    StreamCallbackContext ctx;
    ctx.curl = curl;
    ctx.callback = std::move(chunkCallback);
    ctx.is_cancelled = std::move(isCancelled);

    struct curl_slist *header_list = nullptr;
    header_list = curl_slist_append(header_list, "Content-Type: application/json");
    for (const auto &[key, value] : proxyHeaders) {
      std::string header_line = key + ": " + value;
      header_list = curl_slist_append(header_list, header_line.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, targetUrl.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, processedBody.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(processedBody.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_func_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, stream_progress_callback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);

    CURLcode curlResult = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(header_list);
    curl_easy_cleanup(curl);

    if (curlResult != CURLE_OK) {
      return { 502, R"({"error":{"message":"Failed to connect to upstream LLM API","type":"upstream_error"}})", "application/json" };
    }
    if (http_code < 200 || http_code >= 300) {
      const int status = http_code == 401 || http_code == 429 ? static_cast<int>(http_code) : 502;
      return { status, std::move(ctx.error_body), "application/json" };
    }
    return { 200, "", "text/event-stream" };
  }

}  // namespace ai
