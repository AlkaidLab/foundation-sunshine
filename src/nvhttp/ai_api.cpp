#include "ai_api.h"

#include <sstream>
#include <string>

#include <Simple-Web-Server/server_http.hpp>
#include <nlohmann/json.hpp>

#include "src/ai/http_stream.h"
#include "src/ai/proxy.h"

namespace nvhttp::ai_api {

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
    catch (...) {
    }

    if (isStream) {
      ai::http::stream_response(response, std::move(requestBody));
    }
    else {
      auto result = ai::processAiChat(requestBody);

      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", result.contentType);

      auto statusCode = SimpleWeb::StatusCode::success_ok;
      if (result.httpCode == 403)
        statusCode = SimpleWeb::StatusCode::client_error_forbidden;
      else if (result.httpCode == 400)
        statusCode = SimpleWeb::StatusCode::client_error_bad_request;
      else if (result.httpCode >= 500)
        statusCode = SimpleWeb::StatusCode::server_error_bad_gateway;

      response->write(statusCode, result.body, headers);
    }
  }

}  // namespace nvhttp::ai_api
