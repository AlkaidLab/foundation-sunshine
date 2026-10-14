#include "ai_api.h"

#include <sstream>
#include <string>

#include "src/http/server.h"
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

      web::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", result.contentType);

      auto statusCode = web::StatusCode::ok;
      if (result.httpCode == 403)
        statusCode = web::StatusCode::forbidden;
      else if (result.httpCode == 400)
        statusCode = web::StatusCode::bad_request;
      else if (result.httpCode >= 500)
        statusCode = web::StatusCode::bad_gateway;

      response->write(statusCode, result.body, headers);
    }
  }

}  // namespace nvhttp::ai_api
