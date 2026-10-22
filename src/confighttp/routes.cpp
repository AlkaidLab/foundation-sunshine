/**
 * @file src/confighttp/routes.cpp
 * @brief Web UI route composition and specialized local access policies.
 */
#include "src/confighttp/routes.h"
#include "src/ai/http.h"
#include "src/clipboard_http.h"
#include "src/config.h"
#include "src/confighttp/apps.h"
#include "src/confighttp/assets.h"
#include "src/confighttp/auth.h"
#include "src/confighttp/clients.h"
#include "src/confighttp/common.h"
#include "src/confighttp/configuration.h"
#include "src/confighttp/diagnostics.h"
#include "src/confighttp/integrations.h"
#include "src/confighttp/proxies.h"
#include "src/confighttp/runtime.h"
#include "src/file_mapping/config_http.h"
#include "src/network.h"
#include "src/text_context/http.h"
#include "src/tray/tray_http.h"
#include "src/widget_http.h"

namespace confighttp {
  void
  register_routes(https_server_t &server) {
    server.default_resource["GET"] = close_connection;
    confighttp::auth::register_routes(server);
    confighttp::assets::register_routes(server);
    confighttp::apps::register_routes(server);
    confighttp::configuration::register_routes(server);
    confighttp::clients::register_routes(server);
    confighttp::diagnostics::register_routes(server);
    confighttp::runtime::register_routes(server);
    confighttp::integrations::register_routes(server);
    confighttp::proxies::register_routes(server);
    file_mapping::config_http::register_routes(server);
    ai::http::register_routes(server);

    // Clipboard sync routes are registered in a sibling module so the
    // Web UI server doesn't grow further. They reuse our
    // basic-auth via the lambda below.
    clipboard_http::register_routes(server,
      [](clipboard_http::resp_https_t resp, clipboard_http::req_https_t req) {
        return authenticate(std::move(resp), std::move(req));
      });
    text_context::http::register_routes(server,
      [](text_context::http::resp_https_t resp, text_context::http::req_https_t req) {
        const auto address = net::addr_to_normalized_string(req->remote_endpoint().address());
        if (net::from_address(address) != net::PC) {
          resp->write(web::StatusCode::forbidden);
          return false;
        }
        return authenticate(std::move(resp), std::move(req));
      });
    tray_http::auth_fn tray_local_auth = [](tray_http::resp_https_t resp, tray_http::req_https_t req) {
      const auto address = net::addr_to_normalized_string(req->remote_endpoint().address());
      if (config::sunshine.username.empty() && net::from_address(address) == net::PC) {
        return authorize_browser_request(std::move(resp), std::move(req));
      }
      return authenticate(std::move(resp), std::move(req));
    };
    tray_http::register_routes(server, tray_local_auth, tray_local_auth);
    // Game Bar widget 通道:仅环回,凭证由模块内的 X-Sunshine-Token(widget_token)校验,
    // 不复用 basic-auth——widget 是本地 packaged app,没有浏览器语义。
    widget_http::register_routes(server, [](widget_http::resp_https_t resp, widget_http::req_https_t req) {
      const auto address = net::addr_to_normalized_string(req->remote_endpoint().address());
      if (net::from_address(address) != net::PC) {
        resp->write(web::StatusCode::forbidden);
        return false;
      }
      return true;
    });
  }
}  // namespace confighttp
