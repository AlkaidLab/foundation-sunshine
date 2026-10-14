/**
 * @file src/confighttp/auth.cpp
 * @brief Web UI auth handlers and routes.
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

#include "auth.h"
#include "common.h"
#include "src/config.h"
#include "src/crypto.h"
#include "src/http_util.h"
#include "src/httpcommon.h"
#include "src/logging.h"
#include "src/network.h"
#include "src/utility.h"
#include "src/http/types.h"
#include <boost/algorithm/string.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <optional>
#include <sstream>

using namespace std::literals;
using json = nlohmann::json;

namespace confighttp {
  namespace pt = boost::property_tree;

  namespace {
    struct request_header_value_t {
      bool valid;
      std::optional<std::string_view> value;
    };

    request_header_value_t
    get_single_request_header(const req_https_t &request, const std::string_view name) {
      const auto range = request->header.equal_range(std::string { name });
      if (range.first == range.second) {
        return { true, std::nullopt };
      }

      auto first = range.first;
      const auto value = std::string_view { first->second };
      if (++first != range.second) {
        return { false, std::nullopt };
      }
      return { true, value };
    }

    void
    send_cross_site_forbidden(resp_https_t response) {
      const nlohmann::json body {
        { "status", false },
        { "error", "Cross-site request denied" },
      };
      web::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "application/json");
      headers.emplace("X-Frame-Options", "DENY");
      headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
      response->write(web::StatusCode::forbidden, body.dump(), headers);
    }

  }  // namespace

  bool
  authorize_browser_request(resp_https_t response, req_https_t request) {
    const auto path = std::string_view { request->path };
    if (!path.starts_with("/api/") &&
        !path.starts_with("/steam-api/") &&
        !path.starts_with("/steam-store/")) {
      return true;
    }

    const auto origin = get_single_request_header(request, "origin");
    const auto referer = get_single_request_header(request, "referer");
    const auto fetch_site = get_single_request_header(request, "sec-fetch-site");
    const auto host = get_single_request_header(request, "host");
    const auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    if (!origin.valid || !referer.valid || !fetch_site.valid || !host.valid ||
        !http_util::browser_request_source_allowed(
          origin.value,
          referer.value,
          fetch_site.value,
          host.value.value_or(std::string_view {}),
          net::from_address(address) == net::PC)) {
      BOOST_LOG(debug) << "Web UI: rejected cross-site browser request to ["sv << request->path << ']';
      send_cross_site_forbidden(std::move(response));
      return false;
    }
    return true;
  }

  static void
  send_unauthorized(resp_https_t response, req_https_t request) {
    auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    BOOST_LOG(error) << "Web UI: ["sv << address << "] -- not authorized"sv;
    const web::CaseInsensitiveMultimap headers {
      { "WWW-Authenticate", R"(Basic realm="Sunshine Gamestream Host", charset="UTF-8")" }
    };
    response->write(web::StatusCode::unauthorized, headers);
  }

  /**
   * Logout endpoint: for localhost (PC) returns 200 so the browser does not show
   * a password dialog; for other allowed origins returns 401 with WWW-Authenticate
   * so the browser shows the credential prompt. Denied origins receive 403.
   */
  static void
  handleLogout(resp_https_t response, req_https_t request) {
    if (!authorize_browser_request(response, request)) return;

    auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    auto ip_type = net::from_address(address);

    if (ip_type > http::origin_web_ui_allowed) {
      BOOST_LOG(error) << "Web UI: ["sv << address << "] -- logout denied"sv;
      response->write(web::StatusCode::forbidden);
      return;
    }

    if (ip_type == net::PC) {
      BOOST_LOG(info) << "Web UI: ["sv << address << "] -- logout (local), responding 200"sv;
      response->write(web::StatusCode::ok, "");
      return;
    }

    BOOST_LOG(info) << "Web UI: ["sv << address << "] -- logout requested, responding 401"sv;
    send_unauthorized(response, request);
  }

  void
  send_redirect(resp_https_t response, req_https_t request, const char *path) {
    auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    BOOST_LOG(error) << "Web UI: ["sv << address << "] -- not authorized, redirect"sv;
    const web::CaseInsensitiveMultimap headers {
      { "Location", path }
    };
    response->write(web::StatusCode::temporary_redirect, headers);
  }

  bool
  authenticate(resp_https_t response, req_https_t request) {
    if (!authorize_browser_request(response, request)) return false;

    auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    auto ip_type = net::from_address(address);

    if (ip_type > http::origin_web_ui_allowed) {
      BOOST_LOG(error) << "Web UI: ["sv << address << "] -- denied"sv;
      response->write(web::StatusCode::forbidden);
      return false;
    }

    // If credentials are shown, redirect the user to a /welcome page
    if (config::sunshine.username.empty()) {
      send_redirect(response, request, "/welcome");
      return false;
    }

    if (ip_type == net::PC) {
      return true;
    }

    auto fg = util::fail_guard([&]() {
      send_unauthorized(response, request);
    });

    auto auth = request->header.find("authorization");
    if (auth == request->header.end()) {
      return false;
    }

    auto &rawAuth = auth->second;
    constexpr auto basicPrefix = "Basic "sv;
    if (rawAuth.length() <= basicPrefix.length() ||
        rawAuth.substr(0, basicPrefix.length()) != basicPrefix) {
      return false;
    }

    std::string authData;
    try {
      authData = web::decode_base64(rawAuth.substr(basicPrefix.length()));
    }
    catch (const std::exception &e) {
      BOOST_LOG(debug) << "Authentication: Base64 decode failed: " << e.what();
      return false;
    }

    int index = authData.find(':');
    if (index >= authData.size() - 1) {
      return false;
    }

    auto username = authData.substr(0, index);
    auto password = authData.substr(index + 1);
    auto hash = util::hex(crypto::hash(password + config::sunshine.salt)).to_string();

    if (!boost::iequals(username, config::sunshine.username) || hash != config::sunshine.password) {
      return false;
    }

    fg.disable();
    return true;
  }

  static void
  savePassword(resp_https_t response, req_https_t request) {
    if (!authorize_browser_request(response, request)) return;
    if (!check_content_type(response, request, "application/json")) return;
    if (!config::sunshine.username.empty() && !authenticate(response, request)) return;

    print_req(request);

    std::stringstream ss;
    std::stringstream configStream;
    ss << request->content.rdbuf();

    pt::ptree inputTree, outputTree;

    auto g = util::fail_guard([&]() {
      std::ostringstream data;
      pt::write_json(data, outputTree);
      response->write(data.str());
    });

    try {
      pt::read_json(ss, inputTree);
      auto username = inputTree.count("currentUsername") > 0 ? inputTree.get<std::string>("currentUsername") : "";
      auto newUsername = inputTree.get<std::string>("newUsername");
      auto password = inputTree.count("currentPassword") > 0 ? inputTree.get<std::string>("currentPassword") : "";
      auto newPassword = inputTree.count("newPassword") > 0 ? inputTree.get<std::string>("newPassword") : "";
      auto confirmPassword = inputTree.count("confirmNewPassword") > 0 ? inputTree.get<std::string>("confirmNewPassword") : "";

      // Validate credential lengths
      if (newUsername.size() > 128) {
        outputTree.put("status", false);
        outputTree.put("error", "Username too long (max 128)");
        return;
      }
      if (newPassword.size() > 256) {
        outputTree.put("status", false);
        outputTree.put("error", "Password too long (max 256)");
        return;
      }

      if (newUsername.length() == 0) newUsername = username;
      if (newUsername.length() == 0) {
        outputTree.put("status", false);
        outputTree.put("error", "Invalid Username");
      }
      else {
        auto hash = util::hex(crypto::hash(password + config::sunshine.salt)).to_string();
        if (config::sunshine.username.empty() || (boost::iequals(username, config::sunshine.username) && hash == config::sunshine.password)) {
          if (newPassword.empty() || newPassword != confirmPassword) {
            outputTree.put("status", false);
            outputTree.put("error", "Password Mismatch");
          }
          else {
            if (http::save_user_creds(config::sunshine.credentials_file, newUsername, newPassword) != 0 ||
                http::reload_user_creds(config::sunshine.credentials_file) != 0) {
              outputTree.put("status", false);
              outputTree.put("error", "Failed to save credentials");
            }
            else {
              outputTree.put("status", true);
            }
          }
        }
        else {
          outputTree.put("status", false);
          outputTree.put("error", "Invalid Current Credentials");
        }
      }
    }
    catch (std::exception &e) {
      BOOST_LOG(warning) << "SavePassword: "sv << e.what();
      outputTree.put("status", false);
      outputTree.put("error", e.what());
      return;
    }
  }

  bool
  require_localhost(resp_https_t response, req_https_t request, const std::string &action) {
    auto client_address = request->remote_endpoint().address();
    auto address = net::addr_to_normalized_string(client_address);
    auto ip_type = net::from_address(address);

    if (ip_type == net::PC) {
      return true;
    }

    std::ostringstream msg_stream;
    msg_stream << "Access denied when " << action << ". Only localhost requests are allowed. Client IP: " << client_address.to_string();
    BOOST_LOG(warning) << msg_stream.str();
    write_runtime_error(response, web::StatusCode::forbidden, 403, msg_stream.str());
    return false;
  }

  namespace auth {
    void
    register_routes(https_server_t &server) {
      server.resource["^/api/logout$"]["GET"] = handleLogout;
      server.resource["^/api/logout$"]["POST"] = handleLogout;
      server.resource["^/api/password$"]["POST"] = savePassword;
    }
  }  // namespace auth

}  // namespace confighttp
