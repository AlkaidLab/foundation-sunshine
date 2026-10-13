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
      std::function<void(const char *, size_t)> callback;
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
  stream_func_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t realsize = size * nmemb;
    auto *ctx = static_cast<StreamCallbackContext *>(userdata);
    ctx->callback(ptr, realsize);
    return realsize;
  }

  AiProxyResult
  processAiChatStream(
    const std::string &requestBody,
    std::function<void(const char *, size_t)> chunkCallback) {
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
    ctx.callback = std::move(chunkCallback);

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
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);

    CURLcode curlResult = curl_easy_perform(curl);
    curl_slist_free_all(header_list);
    curl_easy_cleanup(curl);

    if (curlResult != CURLE_OK) {
      return { 502, R"({"error":{"message":"Failed to connect to upstream LLM API","type":"upstream_error"}})", "application/json" };
    }
    return { 200, "", "text/event-stream" };
  }

}  // namespace ai
