/**
 * @file src/ai/http.cpp
 * @brief Web UI ai_http handlers and routes.
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

#include "http.h"
#include "config_store.h"
#include "provider.h"
#include "proxy.h"
#include "src/ai/http_stream.h"
#include "src/confighttp/auth.h"
#include "src/confighttp/common.h"
#include "src/httpcommon.h"
#include <sstream>

using namespace std::literals;
using json = nlohmann::json;

namespace ai::http {

  using namespace confighttp;

  namespace {
    /**
     * @brief GET /api/ai/config — 获取 AI 配置（不返回完整 API key）
     */
    void
    getAiConfig(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;
      print_req(request);

      auto cfg = ai::loadAiConfig();

      // Never expose the key; only report whether one is configured.
      const std::string key = cfg.value("apiKey", "");
      cfg.erase("apiKey");
      cfg["apiKeyConfigured"] = !key.empty();

      web::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "application/json");
      response->write(web::StatusCode::ok, cfg.dump(), headers);
    }

    /**
     * @brief POST /api/ai/config — 保存 AI 配置
     */
    void
    saveAiConfigEndpoint(resp_https_t response, req_https_t request) {
      if (!check_content_type(response, request, "application/json")) return;
      if (!authenticate(response, request)) return;
      print_req(request);

      std::stringstream ss;
      ss << request->content.rdbuf();

      const auto output = ai::save_config(ss.str());

      web::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "application/json");
      response->write(web::StatusCode::ok, output.dump(), headers);
    }

    void
    proxyAiModels(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;
      print_req(request);

      const auto cfg = ai::loadAiConfig();
      if (!cfg.value("enabled", false)) {
        response->write(
          web::StatusCode::forbidden,
          R"({"error":{"message":"AI proxy is not enabled","type":"invalid_request_error"}})",
          json_headers());
        return;
      }
      const std::string api_key = cfg.value("apiKey", "");
      std::string api_base = cfg.value("apiBase", "");
      if (api_base.empty() || (api_key.empty() && ai::isApiKeyRequired(cfg))) {
        response->write(
          web::StatusCode::bad_request,
          R"({"error":{"message":"AI proxy not configured","type":"invalid_request_error"}})",
          json_headers());
        return;
      }
      if (ai::isAnthropicProvider(cfg)) {
        response->write(web::StatusCode::ok, R"({"data":[]})", json_headers());
        return;
      }

      while (!api_base.empty() && api_base.back() == '/') api_base.pop_back();
      if (ai::hasSuffix(api_base, "/chat/completions")) {
        api_base.resize(api_base.size() - std::string_view { "/chat/completions" }.size());
      }
      const std::string target_url = api_base + "/models";
      std::map<std::string, std::string> headers;
      if (!api_key.empty()) headers["Authorization"] = "Bearer " + api_key;

      std::string body;
      long http_code = 0;
      if (!::http::get_json(target_url, headers, body, http_code)) {
        response->write(
          web::StatusCode::bad_gateway,
          R"({"error":{"message":"Failed to connect to upstream LLM API","type":"upstream_error"}})",
          json_headers());
        return;
      }
      const auto status = http_code >= 200 && http_code < 300 ? web::StatusCode::ok : web::StatusCode::bad_gateway;
      response->write(status, body, json_headers());
    }

    /**
     * @brief POST /api/ai/chat/completions — OpenAI 兼容的 LLM 代理端点
     *
     * 支持普通请求和 SSE 流式请求。
     * 自动适配 Anthropic API 格式。
     * 客户端无需知道 API key 或实际后端，Sunshine 作为透明代理。
     */
    void
    proxyAiChat(resp_https_t response, req_https_t request) {
      if (!check_content_type(response, request, "application/json")) return;
      if (!authenticate(response, request)) return;
      print_req(request);

      std::stringstream ss;
      ss << request->content.rdbuf();
      std::string requestBody = ss.str();

      // 检测是否请求流式输出
      bool isStream = false;
      try {
        auto reqJson = nlohmann::json::parse(requestBody);
        isStream = reqJson.value("stream", false);
      }
      catch (...) {
      }

      if (isStream) {
        ai::http::stream_response(response, std::move(requestBody));
      }
      else {
        auto result = ai::processAiChat(requestBody);
        web::CaseInsensitiveMultimap headers;
        headers.emplace("Content-Type", result.contentType);

        auto statusCode = (result.httpCode == 200) ? web::StatusCode::ok : (result.httpCode == 403) ? web::StatusCode::forbidden :
                                                                                       (result.httpCode == 400)   ? web::StatusCode::bad_request :
                                                                                                                    web::StatusCode::bad_gateway;

        response->write(statusCode, result.body, headers);
      }
    }

    /**
     * @brief OPTIONS handler for CORS preflight on AI endpoints
     */
    void
    handleAiCors(resp_https_t response, req_https_t request) {
      web::CaseInsensitiveMultimap headers;
      headers.emplace("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
      headers.emplace("Access-Control-Allow-Headers", "Content-Type, Authorization");
      response->write(web::StatusCode::no_content, "", headers);
    }

  }  // namespace

  void
  register_routes(https_server_t &server) {
    server.resource["^/api/ai/config$"]["GET"] = getAiConfig;
    server.resource["^/api/ai/config$"]["POST"] = saveAiConfigEndpoint;
    server.resource["^/api/ai/models$"]["GET"] = proxyAiModels;
    server.resource["^/api/ai/chat/completions$"]["POST"] = proxyAiChat;
    server.resource["^/api/ai/chat/completions$"]["OPTIONS"] = handleAiCors;
  }

}  // namespace ai::http
