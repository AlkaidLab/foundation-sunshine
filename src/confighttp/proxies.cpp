/**
 * @file src/confighttp/proxies.cpp
 * @brief Web UI proxies handlers and routes.
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

#include "proxies.h"
#include "src/confighttp/auth.h"
#include "src/confighttp/common.h"
#include "src/http_util.h"
#include "src/httpcommon.h"
#include "src/logging.h"

using namespace std::literals;
using json = nlohmann::json;

namespace confighttp::proxies {

  using namespace confighttp;

  namespace {
    void
    proxySteamApi(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;
      print_req(request);

      // 提取请求路径，移除/steam-api前缀
      std::string path = request->path;
      if (path.find("/steam-api") == 0) {
        path = path.substr(10);  // 移除"/steam-api"前缀
      }

      // 构建目标URL
      std::string targetUrl = "https://api.steampowered.com" + path;

      // 添加查询参数
      if (!request->query_string.empty()) {
        targetUrl += "?" + request->query_string;
      }

      BOOST_LOG(info) << "Steam API proxy request path: " << http_util::sanitize_request_log_value(path);

      // 安全检查：防止SSRF，确保目标主机确实是api.steampowered.com
      if (http::url_get_host(targetUrl) != "api.steampowered.com") {
        BOOST_LOG(warning) << "Blocked Steam API proxy request to unauthorized host";
        response->write(web::StatusCode::bad_request, "Invalid Host");
        return;
      }

      // 使用http模块下载数据
      std::string content;

      try {
        if (http::fetch_url(targetUrl, content)) {
          // 设置响应头
          web::CaseInsensitiveMultimap headers;
          headers.emplace("Content-Type", "application/json");

          response->write(web::StatusCode::ok, content, headers);
        }
        else {
          BOOST_LOG(error) << "Steam API request failed for path: " << http_util::sanitize_request_log_value(path);
          response->write(web::StatusCode::internal_server_error, "Steam API request failed");
        }
      }
      catch (const std::exception &e) {
        BOOST_LOG(error) << "Steam API proxy exception: " << e.what();
        response->write(web::StatusCode::internal_server_error, "Steam API proxy exception");
      }
    }

    void
    proxySteamStore(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;
      print_req(request);

      // 提取请求路径，移除/steam-store前缀
      std::string path = request->path;
      if (path.find("/steam-store") == 0) {
        path = path.substr(12);  // 移除"/steam-store"前缀
      }

      // 构建目标URL
      std::string targetUrl = "https://store.steampowered.com" + path;

      // 添加查询参数
      if (!request->query_string.empty()) {
        targetUrl += "?" + request->query_string;
      }

      BOOST_LOG(info) << "Steam Store proxy request path: " << http_util::sanitize_request_log_value(path);

      // 安全检查：防止SSRF，确保目标主机确实是store.steampowered.com
      if (http::url_get_host(targetUrl) != "store.steampowered.com") {
        BOOST_LOG(warning) << "Blocked Steam Store proxy request to unauthorized host";
        response->write(web::StatusCode::bad_request, "Invalid Host");
        return;
      }

      // 使用http模块下载数据
      std::string content;

      try {
        if (http::fetch_url(targetUrl, content)) {
          // 设置响应头
          web::CaseInsensitiveMultimap headers;
          headers.emplace("Content-Type", "application/json");

          response->write(web::StatusCode::ok, content, headers);
        }
        else {
          BOOST_LOG(error) << "Steam Store request failed for path: " << http_util::sanitize_request_log_value(path);
          response->write(web::StatusCode::internal_server_error, "Steam Store request failed");
        }
      }
      catch (const std::exception &e) {
        BOOST_LOG(error) << "Steam Store proxy exception: " << e.what();
        response->write(web::StatusCode::internal_server_error, "Steam Store proxy exception");
      }
    }

  }  // namespace

  void
  register_routes(https_server_t &server) {
    server.resource["^/steam-api/.+$"]["GET"] = proxySteamApi;
    server.resource["^/steam-store/.+$"]["GET"] = proxySteamStore;
  }

}  // namespace confighttp::proxies
