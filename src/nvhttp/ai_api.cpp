#include "ai_api.h"

#include <sstream>
#include <string>

#include <Simple-Web-Server/server_http.hpp>
#include <nlohmann/json.hpp>

#include "src/confighttp.h"

namespace nvhttp::ai_api {

  namespace {

    const char *
    status_line_for_ai_result(int http_code) {
      if (http_code == 400) return "400 Bad Request";
      if (http_code == 403) return "403 Forbidden";
      if (http_code >= 500) return "502 Bad Gateway";
      return "500 Internal Server Error";
    }

  }  // namespace

  void
  completions(resp_https_t response, req_https_t request) {
    std::stringstream ss;
    ss << request->content.rdbuf();
    std::string requestBody = ss.str();

    bool isStream = false;
    try {
      auto reqJson = nlohmann::json::parse(requestBody);
      isStream = reqJson.value("stream", false);
    }
    catch (...) {}

    if (isStream) {
      bool headerSent = false;
      auto result = confighttp::processAiChatStream(requestBody, [&](const char *data, size_t len) {
        if (!headerSent) {
          *response << "HTTP/1.1 200 OK\r\n";
          *response << "Content-Type: text/event-stream\r\n";
          *response << "Cache-Control: no-cache\r\n";
          *response << "Connection: keep-alive\r\n";
          *response << "\r\n";
          response->send();
          headerSent = true;
        }
        std::string chunk(data, len);
        *response << chunk;
        response->send();
      });

      if (result.httpCode != 200 && !headerSent) {
        std::string errorResp = result.body;
        *response << "HTTP/1.1 " << status_line_for_ai_result(result.httpCode) << "\r\n";
        *response << "Content-Type: application/json\r\n";
        *response << "Content-Length: " << errorResp.size() << "\r\n";
        *response << "\r\n";
        *response << errorResp;
        response->send();
      }
    }
    else {
      auto result = confighttp::processAiChat(requestBody);

      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", result.contentType);

      auto statusCode = SimpleWeb::StatusCode::success_ok;
      if (result.httpCode == 403) statusCode = SimpleWeb::StatusCode::client_error_forbidden;
      else if (result.httpCode == 400) statusCode = SimpleWeb::StatusCode::client_error_bad_request;
      else if (result.httpCode >= 500) statusCode = SimpleWeb::StatusCode::server_error_bad_gateway;

      response->write(statusCode, result.body, headers);
    }
  }

}  // namespace nvhttp::ai_api
