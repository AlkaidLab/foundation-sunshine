/**
 * @file src/confighttp/auth.cpp
 * @brief Web UI authentication, sessions and credential routes.
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

#include "auth.h"
#include "common.h"
#include "local_auth_token.h"
#include "login_throttle.h"
#include "session_store.h"
#include "src/config.h"
#include "src/confighttp.h"
#include "src/crypto.h"
#include "src/file_handler.h"
#include "src/http/types.h"
#include "src/http_util.h"
#include "src/httpcommon.h"
#include "src/logging.h"
#include "src/network.h"
#include "src/utility.h"
#include <algorithm>
#include <boost/algorithm/string.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <openssl/crypto.h>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

using namespace std::literals;
using json = nlohmann::json;

namespace confighttp {
  namespace pt = boost::property_tree;

  namespace {
    // __Host- requires Secure, Path=/ and no Domain attribute, so the cookie is
    // host-only by construction. The port suffix keeps separate host processes
    // from replacing one another's cookies; the default port retains the
    // historical name so an upgrade does not sign out existing browsers.
    // Sources: OWASP ASVS 4.0.3 V3.4.1-V3.4.5
    // [ds5-owasp-asvs4-v3-20261103]; OWASP Session Management Cheat Sheet,
    // Cookie Name Prefixes [ds5-owasp-session-cheatsheet-20261103]; RFC 6265bis
    // sections 4.1.1 and 5.4 [lt3-rfc6265bis-20261107]; Chromium cookie
    // lifecycle [p0-chromium-cookie-readme-20261109]; Firefox cookie guidance
    // [p0-firefox-cookie-guide-20261109]; WebKit ITP cookie expiry guidance
    // [p0-webkit-itp-20261109]. The default service port is the documented
    // GameStream HTTP port [pair-link-moonlight-qt-http-port-20261109].
    constexpr std::uint16_t default_cookie_port = 47989;

    std::string
    cookie_name(const std::string_view base) {
      const auto port = config::sunshine.port == 0 ? default_cookie_port : config::sunshine.port;
      std::string name { "__Host-sunshine_" };
      name.append(base);
      if (port != default_cookie_port) {
        name.append("_");
        name.append(std::to_string(port));
      }
      return name;
    }

    std::string
    session_cookie_name() {
      return cookie_name("session");
    }

    std::string
    csrf_cookie_name() {
      return cookie_name("csrf");
    }
    constexpr std::size_t maximum_json_body = 8 * 1024;
    constexpr std::size_t maximum_username_length = 128;
    constexpr std::size_t minimum_password_length = 12;
    constexpr std::size_t maximum_password_length = 128;

    struct request_header_value_t {
      bool valid;
      std::optional<std::string_view> value;
    };

    bool
    credentials_exist();

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

    bool
    constant_time_equal(const std::string_view left, const std::string_view right) {
      if (left.size() != right.size()) {
        return false;
      }
      // OpenSSL documents CRYPTO_memcmp as the constant-time comparison helper
      // [ds5-openssl-crypto-memcmp-20261103].
      return CRYPTO_memcmp(left.data(), right.data(), left.size()) == 0;
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

    std::string
    mask_username(const std::string_view username) {
      if (username.empty()) {
        return "<empty>";
      }
      if (username.size() <= 2) {
        return std::string(username.size(), '*');
      }
      std::string masked;
      masked.push_back(username.front());
      masked.append("***");
      masked.push_back(username.back());
      return masked;
    }

    std::string
    to_iso8601(const sessions::time_point_t &value) {
      const auto seconds = std::chrono::floor<std::chrono::seconds>(value);
      const std::time_t time = sessions::clock_t::to_time_t(seconds);
      std::tm broken_down {};
#ifdef _WIN32
      gmtime_s(&broken_down, &time);
#else
      gmtime_r(&time, &broken_down);
#endif
      char buffer[32] {};
      std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &broken_down);
      return buffer;
    }

    bool
    is_api_path(const std::string_view path) {
      return path == "/api"sv || path.starts_with("/api/"sv) ||
             path.starts_with("/steam-api/"sv) || path.starts_with("/steam-store/"sv);
    }

    bool
    is_state_changing(const std::string_view method) {
      return method == "POST"sv || method == "PUT"sv || method == "PATCH"sv || method == "DELETE"sv;
    }

    std::optional<std::string>
    get_cookie(const req_https_t &request, const std::string_view name) {
      // Cookie syntax follows RFC 6265bis sections 5.2-5.4
      // [lt3-rfc6265bis-20261107]. Duplicate names are rejected.
      const auto range = request->header.equal_range("cookie");
      std::optional<std::string> result;
      for (auto header = range.first; header != range.second; ++header) {
        const std::string_view value { header->second };
        std::size_t begin = 0;
        while (begin < value.size()) {
          auto end = value.find(';', begin);
          if (end == std::string_view::npos) {
            end = value.size();
          }
          auto pair = value.substr(begin, end - begin);
          while (!pair.empty() && (pair.front() == ' ' || pair.front() == '\t')) {
            pair.remove_prefix(1);
          }
          const auto equals = pair.find('=');
          if (equals != std::string_view::npos) {
            const auto key = pair.substr(0, equals);
            if (key == name) {
              if (result) {
                return std::nullopt;
              }
              result = std::string(pair.substr(equals + 1));
            }
          }
          begin = end + 1;
        }
      }
      return result;
    }

    /**
     * A same-origin return path: a single leading slash and no control
     * characters, backslashes or encoded backslashes, so the value cannot be
     * turned into another origin by a URL parser.
     */
    bool
    is_same_origin_path(const std::string_view target) {
      if (target.size() < 2 || target.front() != '/' || target[1] == '/' || target.find('\\') != std::string_view::npos) {
        return false;
      }

      std::string lowered;
      lowered.reserve(target.size());
      for (const auto character : target) {
        const auto value = static_cast<unsigned char>(character);
        if (value < 0x20 || value == 0x7F) {
          return false;
        }
        lowered.push_back(value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : character);
      }
      return lowered.find("%5c"sv) == std::string::npos;
    }

    std::string
    safe_return_path(const req_https_t &request) {
      if (request->method != "GET"sv) {
        return {};
      }

      auto target = request->path;
      if (!request->query_string.empty()) {
        target.push_back('?');
        target.append(request->query_string);
      }
      return is_same_origin_path(target) ? target : std::string {};
    }

    /**
     * Where an authenticated visit to /login continues to. The `next` query
     * parameter is validated like any return path and must not name the login
     * page itself, or the redirect would loop.
     */
    std::string
    login_return_target(const req_https_t &request) {
      const auto query = request->parse_query_string();
      const auto found = query.find("next");
      if (found == query.end() || !is_same_origin_path(found->second)) {
        return "/";
      }
      const auto target = std::string_view { found->second };
      if (target == "/login" || target.starts_with("/login/") || target.starts_with("/login?")) {
        return "/";
      }
      return std::string { target };
    }

    void
    add_session_cookies(web::CaseInsensitiveMultimap &headers, const sessions::created_session_t &created) {
      const auto lifetime = std::chrono::duration_cast<std::chrono::seconds>(created.absolute_expires_at - created.created_at);
      const auto max_age = std::to_string(std::max<std::int64_t>(0, lifetime.count()));

      std::string session_cookie { session_cookie_name() };
      session_cookie.append("=");
      session_cookie.append(created.token);
      session_cookie.append("; HttpOnly; Secure; SameSite=Strict; Path=/; Max-Age=");
      session_cookie.append(max_age);
      headers.emplace("Set-Cookie", std::move(session_cookie));

      // The CSRF cookie is intentionally readable by the page so it can echo
      // the value in X-CSRF-Token. The server still verifies it against the
      // session record, so this is a session-bound double submit rather than
      // the naive pattern [ds5-owasp-csrf-cheatsheet-20261103].
      std::string csrf_cookie { csrf_cookie_name() };
      csrf_cookie.append("=");
      csrf_cookie.append(created.csrf_token);
      csrf_cookie.append("; Secure; SameSite=Strict; Path=/; Max-Age=");
      csrf_cookie.append(max_age);
      headers.emplace("Set-Cookie", std::move(csrf_cookie));
    }

    void
    add_cleared_session_cookies(web::CaseInsensitiveMultimap &headers) {
      std::string session_cookie { session_cookie_name() };
      session_cookie.append("=; HttpOnly; Secure; SameSite=Strict; Path=/; Max-Age=0; Expires=Thu, 01 Jan 1970 00:00:00 GMT");
      headers.emplace("Set-Cookie", std::move(session_cookie));

      std::string csrf_cookie { csrf_cookie_name() };
      csrf_cookie.append("=; Secure; SameSite=Strict; Path=/; Max-Age=0; Expires=Thu, 01 Jan 1970 00:00:00 GMT");
      headers.emplace("Set-Cookie", std::move(csrf_cookie));
    }

    void
    send_unauthenticated(resp_https_t response, req_https_t request) {
      // 401 for API clients, 303 for a navigation; never a Basic challenge
      // [ds5-rfc9110-http-20261031] sections 15.4.2 and 15.4.3.
      const auto setup_required = !credentials_exist();
      if (is_api_path(request->path)) {
        const nlohmann::json body {
          { "status", false },
          { "error", "Authentication required" },
          { "code", "unauthenticated" },
          { "setup_required", setup_required },
        };
        write_json(std::move(response), web::StatusCode::unauthorized, body);
        return;
      }

      std::string location { "/login" };
      const auto next = safe_return_path(request);
      if (!next.empty()) {
        location.append("?next=");
        location.append(web::Percent::encode(next));
      }
      else if (setup_required) {
        location.append("?setup=1");
      }

      web::CaseInsensitiveMultimap headers {
        { "Location", std::move(location) },
        { "Cache-Control", "no-store" },
      };
      response->write(web::StatusCode::see_other, headers);
    }

    bool
    read_json_request(resp_https_t response, req_https_t request, nlohmann::json &body) {
      if (request->content.size() > maximum_json_body) {
        write_json_error(std::move(response), web::StatusCode::payload_too_large, "Request body too large");
        return false;
      }
      try {
        body = nlohmann::json::parse(request->content.str());
      }
      catch (const std::exception &) {
        write_json_error(std::move(response), web::StatusCode::bad_request, "Invalid JSON");
        return false;
      }
      if (!body.is_object()) {
        write_json_error(std::move(response), web::StatusCode::bad_request, "JSON object required");
        return false;
      }
      return true;
    }

    bool
    read_string(const nlohmann::json &body, const char *name, std::string &value) {
      const auto found = body.find(name);
      if (found == body.end() || !found->is_string()) {
        return false;
      }
      value = found->get<std::string>();
      return true;
    }

    bool
    read_remember(const nlohmann::json &body) {
      const auto found = body.find("remember");
      return found != body.end() && found->is_boolean() && found->get<bool>();
    }

    sessions::client_t
    client_description(const req_https_t &request) {
      const auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
      const auto agent = get_single_request_header(request, "user-agent");
      return {
        .address = address,
        .user_agent = agent.valid && agent.value ? std::string { *agent.value } : std::string {},
      };
    }

    std::mutex credentials_mutex;

    struct credential_snapshot_t {
      std::string username;
      std::string password;
      std::string salt;
    };

    credential_snapshot_t
    snapshot_credentials() {
      std::lock_guard lock(credentials_mutex);
      return { config::sunshine.username, config::sunshine.password, config::sunshine.salt };
    }

    bool
    credentials_exist() {
      std::lock_guard lock(credentials_mutex);
      return !config::sunshine.username.empty() && !config::sunshine.password.empty();
    }

    std::string
    credential_fingerprint() {
      std::lock_guard lock(credentials_mutex);
      return util::hex(crypto::hash(config::sunshine.password)).to_string();
    }

    /**
     * Verify with the existing salted credential store. The password hash is
     * always computed, even for an unknown user, and compared in constant
     * time so a wrong user and a wrong password follow the same path.
     *
     * Source: OWASP ASVS 4.0.3 V2.1, V2.4.2 [ds5-owasp-asvs4-v2-20261103].
     */
    bool
    verify_credentials(const std::string_view username, const std::string_view password) {
      const auto snapshot = snapshot_credentials();
      if (snapshot.username.empty() || snapshot.password.empty()) {
        return false;
      }

      const auto computed = util::hex(crypto::hash(std::string(password) + snapshot.salt)).to_string();
      const auto password_ok = constant_time_equal(computed, snapshot.password);
      const auto username_ok = boost::iequals(username, snapshot.username);
      return password_ok && username_ok;
    }

    bool
    save_credentials_locked(const std::string &username, const std::string &password) {
      return http::save_user_creds(config::sunshine.credentials_file, username, password) == 0 &&
             http::reload_user_creds(config::sunshine.credentials_file) == 0;
    }

    sessions::store_t &
    session_store() {
      static sessions::store_t store;
      const auto config_file = file_handler::path_from_utf8(config::sunshine.config_file);
      const auto persistence_path = (config_file.has_parent_path() ? config_file.parent_path() : std::filesystem::current_path()) /
                                    "web-ui-sessions.json";
      store.configure(
        sessions::policy_t {
          .idle = std::chrono::minutes(config::sunshine.session_idle_minutes),
          .absolute = std::chrono::hours(config::sunshine.session_absolute_hours),
          .remember_idle = std::chrono::minutes(config::sunshine.session_remember_idle_minutes),
          .remember_absolute = std::chrono::hours(24 * config::sunshine.session_remember_days),
        },
        persistence_path,
        local_auth_token::write_restricted_file);
      return store;
    }

    auth::login_throttle_t &
    login_throttle() {
      static auth::login_throttle_t throttle;
      return throttle;
    }

    bool
    is_local(const req_https_t &request) {
      const auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
      return net::from_address(address) == net::PC;
    }

    bool
    address_allowed(const req_https_t &request) {
      const auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
      return net::from_address(address) <= http::origin_web_ui_allowed;
    }

    bool
    csrf_allowed(const req_https_t &request, const std::optional<std::string> &token) {
      if (!token || !is_state_changing(request->method)) {
        return true;
      }

      const auto header = get_single_request_header(request, "x-csrf-token");
      const auto cookie = get_cookie(request, csrf_cookie_name());
      return header.valid && header.value && cookie &&
             session_store().csrf_matches(*token, *header.value, *cookie);
    }

    /**
     * Why a state change that carries a valid session was refused for its CSRF token, for the log (no token value is written). A page that reads another cookie name than the host
     * sets (the names carry the service port) shows up as "no X-CSRF-Token header", which is what the 5 s lease of the home page repeated in the log of a host on port 57989.
     */
    std::string_view
    csrf_failure_reason(const req_https_t &request) {
      const auto header = get_single_request_header(request, "x-csrf-token");
      if (!header.valid) {
        return "duplicate X-CSRF-Token headers"sv;
      }
      if (!header.value) {
        return "no X-CSRF-Token header (the page did not read its CSRF cookie)"sv;
      }
      if (!get_cookie(request, csrf_cookie_name())) {
        return "no CSRF cookie in the request (the browser dropped it)"sv;
      }
      return "the header differs from the cookie, or the token is not the one of this session"sv;
    }

    void
    write_session_json(nlohmann::json &body, const sessions::session_t &session, const std::optional<std::string> &csrf_token) {
      // The names of this host's cookies carry its service port unless that is the default; a page can read them from here instead of deriving them
      body["session_cookie"] = session_cookie_name();
      body["csrf_cookie"] = csrf_cookie_name();
      body["authenticated"] = true;
      body["username"] = session.username;
      body["session_id"] = session.public_id;
      body["created_at"] = to_iso8601(session.created_at);
      body["last_seen"] = to_iso8601(session.last_seen);
      body["expires_at"] = to_iso8601(session.expires_at);
      body["absolute_expires_at"] = to_iso8601(session.absolute_expires_at);
      body["remember"] = session.remember;
      body["local"] = false;
      body["setup_required"] = false;
      if (csrf_token) {
        body["csrf_token"] = *csrf_token;
      }
    }

    /**
     * Legacy logout endpoint. It used to answer a remote browser with 401 and
     * WWW-Authenticate, which is the Basic popup this change removes. It now
     * revokes the session if one is presented and always answers 200.
     */
    void
    handleLogout(resp_https_t response, req_https_t request) {
      if (!authorize_browser_request(response, request)) return;

      const auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
      if (!address_allowed(request)) {
        BOOST_LOG(error) << "Web UI: ["sv << address << "] -- logout denied"sv;
        response->write(web::StatusCode::forbidden);
        return;
      }

      if (const auto token = get_cookie(request, session_cookie_name())) {
        session_store().revoke(*token);
      }
      web::CaseInsensitiveMultimap headers;
      add_cleared_session_cookies(headers);
      BOOST_LOG(info) << "Web UI: ["sv << address << "] -- logout"sv;
      response->write(web::StatusCode::ok, "", headers);
    }

    static void
    savePassword(resp_https_t response, req_https_t request) {
      if (!authorize_browser_request(response, request)) return;
      if (!check_content_type(response, request, "application/json")) return;
      if (credentials_exist() && !authenticate(response, request)) return;

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
          const auto snapshot = snapshot_credentials();
          const auto hash = util::hex(crypto::hash(std::string(password) + snapshot.salt)).to_string();
          if (snapshot.username.empty() || (boost::iequals(username, snapshot.username) && constant_time_equal(hash, snapshot.password))) {
            if (newPassword.empty() || newPassword != confirmPassword) {
              outputTree.put("status", false);
              outputTree.put("error", "Password Mismatch");
            }
            else {
              bool saved = false;
              {
                std::lock_guard lock(credentials_mutex);
                saved = save_credentials_locked(newUsername, newPassword);
              }
              if (!saved) {
                outputTree.put("status", false);
                outputTree.put("error", "Failed to save credentials");
              }
              else {
                // Any credential change invalidates every session that was
                // created under the previous credential.
                session_store().revoke_all();
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

    void
    send_login_page(resp_https_t response, req_https_t request) {
      if (!authorize_browser_request(response, request)) return;
      if (!address_allowed(request)) {
        response->write(web::StatusCode::forbidden);
        return;
      }

      if (const auto token = get_cookie(request, session_cookie_name())) {
        if (session_store().lookup(*token, credential_fingerprint())) {
          // Already signed in: continue past the login page to the validated
          // return target. The target never names /login, so a bookmarked
          // sign-in link cannot redirect to itself.
          web::CaseInsensitiveMultimap headers {
            { "Location", login_return_target(request) },
            { "Cache-Control", "no-store" },
          };
          response->write(web::StatusCode::see_other, headers);
          return;
        }
      }

      const auto page = file_handler::path_from_utf8(std::string(WEB_DIR) + "login.html");
      std::error_code error;
      if (std::filesystem::is_regular_file(page, error)) {
        if (response->write_file(web::StatusCode::ok, page, {
                                                              { "Content-Type", "text/html; charset=utf-8" },
                                                              { "Cache-Control", "no-store" },
                                                              { "X-Frame-Options", "DENY" },
                                                            })) {
          return;
        }
        BOOST_LOG(warning) << "Web UI: could not serve login.html"sv;
      }

      // The Web UI build replaces this minimal page with the Material login
      // view. Keeping a server-side fallback means a fresh checkout still has
      // a working sign-in path.
      constexpr auto fallback = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Sign in</title>
<style>
body{font-family:system-ui,sans-serif;margin:0;min-height:100vh;display:grid;place-items:center;background:#f4f6f8;color:#1d1d1f}
main{width:min(92vw,360px);background:#fff;padding:28px;border-radius:16px;box-shadow:0 12px 32px #0002}
label{display:block;margin:14px 0 6px;font-size:14px}
input{width:100%;box-sizing:border-box;padding:11px;border:1px solid #b9c0c7;border-radius:9px;font:inherit}
button{width:100%;margin-top:20px;padding:12px;border:0;border-radius:9px;background:#0b57d0;color:#fff;font:inherit;font-weight:600}
#error{color:#b3261e;min-height:1.4em;font-size:14px}
</style>
</head>
<body><main>
<h1 id="title">Sign in</h1>
<form id="form">
<label for="username">Username</label><input id="username" autocomplete="username" required maxlength="128">
<label for="password">Password</label><input id="password" type="password" autocomplete="current-password" required maxlength="128">
<div id="confirm-row" hidden><label for="confirm">Confirm password</label><input id="confirm" type="password" autocomplete="new-password" maxlength="128"></div>
<label><input id="remember" type="checkbox" style="width:auto"> Keep me signed in</label>
<p id="error" role="alert"></p>
<button id="submit" type="submit">Sign in</button>
</form>
</main>
<script>
const params=new URLSearchParams(location.search);
const target=params.get('next');
const next=target&&target.startsWith('/')&&!target.startsWith('//')?target:'/';
const form=document.getElementById('form'),error=document.getElementById('error'),submit=document.getElementById('submit'),confirmRow=document.getElementById('confirm-row');
let setup=false;
async function load(){try{const r=await fetch('/api/auth/session',{headers:{'Accept':'application/json'}});const s=await r.json();setup=!!s.setup_required;if(setup){document.getElementById('title').textContent='Create account';document.getElementById('password').autocomplete='new-password';confirmRow.hidden=false;submit.textContent='Create account';}}catch(e){}}
form.addEventListener('submit',async e=>{e.preventDefault();error.textContent='';const username=document.getElementById('username').value;const password=document.getElementById('password').value;if(setup&&password!==document.getElementById('confirm').value){error.textContent='Passwords do not match';return;}submit.disabled=true;try{const r=await fetch(setup?'/api/auth/setup':'/api/auth/login',{method:'POST',headers:{'Content-Type':'application/json','Accept':'application/json'},body:JSON.stringify({username,password,remember:document.getElementById('remember').checked})});const data=await r.json().catch(()=>({}));if(!r.ok){error.textContent=data.error||'Sign in failed';return;}location.assign(next);}catch(e){error.textContent='Sign in failed';}finally{submit.disabled=false;}});
load();
</script>
</body></html>)HTML";
      const web::CaseInsensitiveMultimap headers {
        { "Content-Type", "text/html; charset=utf-8" },
        { "Cache-Control", "no-store" },
        { "X-Frame-Options", "DENY" },
        { "Content-Security-Policy", "default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; connect-src 'self'; form-action 'self'; frame-ancestors 'none'; base-uri 'none'" },
      };
      response->write(web::StatusCode::ok, fallback, headers);
    }

    void
    handleLogin(resp_https_t response, req_https_t request) {
      if (!authorize_browser_request(response, request)) return;
      if (!address_allowed(request)) {
        response->write(web::StatusCode::forbidden);
        return;
      }
      if (!check_content_type(response, request, "application/json")) return;

      nlohmann::json body;
      if (!read_json_request(response, request, body)) return;

      std::string username;
      std::string password;
      if (!read_string(body, "username", username) || !read_string(body, "password", password) ||
          username.empty() || username.size() > maximum_username_length) {
        write_json_error(std::move(response), web::StatusCode::bad_request, "Invalid credentials request");
        return;
      }

      const auto client = client_description(request);
      const auto throttle = login_throttle().check(client.address, username);
      if (!throttle.allowed) {
        web::CaseInsensitiveMultimap headers = json_headers();
        headers.emplace("Retry-After", std::to_string(throttle.retry_after.count()));
        nlohmann::json wait {
          { "status", false },
          { "error", "Too many attempts" },
          { "code", "rate_limited" },
          { "retry_after", throttle.retry_after.count() },
        };
        response->write(web::StatusCode::too_many_requests, wait.dump(), headers);
        return;
      }

      if (!verify_credentials(username, password)) {
        login_throttle().record_failure(client.address, username);
        BOOST_LOG(info) << "Web UI: ["sv << client.address << "] login failed for ["sv << mask_username(username) << ']';
        write_json_error(std::move(response), web::StatusCode::unauthorized, "Invalid username or password");
        return;
      }

      login_throttle().record_success(client.address, username);

      const auto previous = get_cookie(request, session_cookie_name());
      auto created = session_store().create(
        username,
        read_remember(body),
        client,
        credential_fingerprint(),
        previous ? std::optional<std::string_view> { *previous } : std::nullopt);
      if (!created) {
        write_json_error(std::move(response), web::StatusCode::internal_server_error, "Could not create a session");
        return;
      }

      web::CaseInsensitiveMultimap headers = json_headers();
      add_session_cookies(headers, *created);
      nlohmann::json result {
        { "status", true },
        { "authenticated", true },
        { "username", created->username },
        { "session_id", created->public_id },
        { "created_at", to_iso8601(created->created_at) },
        { "expires_at", to_iso8601(created->expires_at) },
        { "absolute_expires_at", to_iso8601(created->absolute_expires_at) },
        { "remember", created->remember },
        { "csrf_token", created->csrf_token },
      };
      response->write(web::StatusCode::ok, result.dump(), headers);
    }

    void
    handleSetup(resp_https_t response, req_https_t request) {
      if (!authorize_browser_request(response, request)) return;
      if (!address_allowed(request)) {
        response->write(web::StatusCode::forbidden);
        return;
      }
      if (!check_content_type(response, request, "application/json")) return;

      nlohmann::json body;
      if (!read_json_request(response, request, body)) return;

      std::string username;
      std::string password;
      if (!read_string(body, "username", username) || !read_string(body, "password", password) ||
          username.empty() || username.size() > maximum_username_length) {
        write_json_error(std::move(response), web::StatusCode::bad_request, "Invalid setup request");
        return;
      }
      if (password.size() < minimum_password_length || password.size() > maximum_password_length) {
        write_json_error(std::move(response), web::StatusCode::bad_request,
          "Password must be between 12 and 128 characters");
        return;
      }

      enum class setup_result_e { ok,
        already_done,
        failed };
      auto setup_result = setup_result_e::ok;
      {
        std::lock_guard lock(credentials_mutex);
        if (http::user_creds_exist(config::sunshine.credentials_file) || !config::sunshine.username.empty()) {
          setup_result = setup_result_e::already_done;
        }
        else if (!save_credentials_locked(username, password)) {
          setup_result = setup_result_e::failed;
        }
      }
      if (setup_result == setup_result_e::already_done) {
        write_json_error(std::move(response), web::StatusCode::conflict, "Setup already completed");
        return;
      }
      if (setup_result == setup_result_e::failed) {
        write_json_error(std::move(response), web::StatusCode::internal_server_error, "Failed to save credentials");
        return;
      }

      const auto client = client_description(request);
      auto created = session_store().create(username, read_remember(body), client, credential_fingerprint());
      if (!created) {
        write_json_error(std::move(response), web::StatusCode::internal_server_error, "Could not create a session");
        return;
      }

      web::CaseInsensitiveMultimap headers = json_headers();
      add_session_cookies(headers, *created);
      nlohmann::json result {
        { "status", true },
        { "authenticated", true },
        { "username", created->username },
        { "session_id", created->public_id },
        { "created_at", to_iso8601(created->created_at) },
        { "expires_at", to_iso8601(created->expires_at) },
        { "absolute_expires_at", to_iso8601(created->absolute_expires_at) },
        { "remember", created->remember },
        { "csrf_token", created->csrf_token },
      };
      response->write(web::StatusCode::ok, result.dump(), headers);
    }

    void
    handleSessionInfo(resp_https_t response, req_https_t request) {
      if (!authorize_browser_request(response, request)) return;
      if (!address_allowed(request)) {
        response->write(web::StatusCode::forbidden);
        return;
      }

      nlohmann::json body;
      const auto token = get_cookie(request, session_cookie_name());
      if (token) {
        if (const auto resumed = session_store().resume(*token, credential_fingerprint())) {
          write_session_json(body, *resumed, resumed->csrf_token);
          web::CaseInsensitiveMultimap headers = json_headers();
          add_session_cookies(headers, *resumed);
          response->write(web::StatusCode::ok, body.dump(), headers);
          return;
        }
        if (const auto session = session_store().lookup(*token, credential_fingerprint())) {
          const auto csrf = get_cookie(request, csrf_cookie_name());
          const auto valid_csrf = csrf && session_store().csrf_matches(*token, *csrf, *csrf) ? std::optional<std::string> { *csrf } : std::nullopt;
          write_session_json(body, *session, valid_csrf);
          write_json(std::move(response), web::StatusCode::ok, body);
          return;
        }
      }

      // The desktop panel on this computer holds the local credential (the header its loopback proxy adds, which only it can read from the ACL-protected file). It is trusted without a
      // session, and the answer says so: `local` is true, the account is named, and the page shows the account and the password form instead of a signed-out state with a sign-in that
      // cannot work in the panel. A request that carries a forwarding header is not the panel (a reverse proxy on this computer), as for the password reset.
      {
        const auto local_token = get_single_request_header(request, local_auth_token::header_name);
        const bool forwarded = request->header.find("x-forwarded-for") != request->header.end() ||
                               request->header.find("forwarded") != request->header.end() ||
                               request->header.find("x-real-ip") != request->header.end();
        if (!forwarded && is_local(request) && local_token.valid && local_token.value &&
            local_auth_token::matches(*local_token.value) && credentials_exist()) {
          const auto snapshot = snapshot_credentials();
          body["authenticated"] = true;
          body["local"] = true;
          body["username"] = snapshot.username;
          body["setup_required"] = false;
          write_json(std::move(response), web::StatusCode::ok, body);
          return;
        }
      }

      // A missing cookie is signed out even on loopback. API callers must
      // present a session cookie, Basic credentials, or the local token through
      // authenticate(); this probe must not claim a local browser is signed in
      // when its next API request will receive 401.
      body["authenticated"] = false;
      body["setup_required"] = !credentials_exist();
      write_json(std::move(response), web::StatusCode::ok, body);
    }

    void
    handleSessionLogout(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;

      const auto token = get_cookie(request, session_cookie_name());
      if (token) {
        session_store().revoke(*token);
      }
      web::CaseInsensitiveMultimap headers = json_headers();
      add_cleared_session_cookies(headers);
      response->write(web::StatusCode::ok, nlohmann::json { { "status", true } }.dump(), headers);
    }

    void
    handleSessionList(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;

      const auto token = get_cookie(request, session_cookie_name());
      const auto current = token ? session_store().lookup(*token, credential_fingerprint()) : std::nullopt;
      nlohmann::json list = nlohmann::json::array();
      for (const auto &summary : session_store().list(token ? *token : "")) {
        list.push_back({
          { "id", summary.public_id },
          { "created_at", to_iso8601(summary.created_at) },
          { "last_seen", to_iso8601(summary.last_seen) },
          { "expires_at", to_iso8601(summary.expires_at) },
          { "client_ip", summary.masked_address },
          { "user_agent", summary.short_user_agent },
          { "current", summary.current },
          { "remember", summary.remember },
        });
      }

      nlohmann::json body {
        { "status", true },
        { "sessions", std::move(list) },
      };
      if (current) {
        body["current"] = current->public_id;
      }
      write_json(std::move(response), web::StatusCode::ok, body);
    }

    void
    handleSessionDelete(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;

      const auto token = get_cookie(request, session_cookie_name());
      const auto query = request->parse_query_string();
      const auto all = query.find("all");
      std::size_t revoked = 0;
      bool clears_current = false;

      if (all != query.end() && (all->second == "1" || boost::iequals(all->second, "true"))) {
        revoked = session_store().revoke_all();
        clears_current = true;
      }
      else if (request->path_match.size() > 1) {
        const auto id = request->path_match[1].str();
        if (id.size() == 32) {
          revoked = session_store().revoke_public(id) ? 1 : 0;
          clears_current = revoked != 0 && token &&
                           session_store().lookup(*token, credential_fingerprint()) == std::nullopt;
        }
      }

      web::CaseInsensitiveMultimap headers = json_headers();
      if (clears_current) {
        add_cleared_session_cookies(headers);
      }
      nlohmann::json body {
        { "status", true },
        { "revoked", revoked },
      };
      response->write(web::StatusCode::ok, body.dump(), headers);
    }

    void
    handlePasswordChange(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;
      if (!check_content_type(response, request, "application/json")) return;

      const auto snapshot = snapshot_credentials();
      const auto client = client_description(request);

      // The current password is verified on every change, so the same
      // anti-automation backoff as login protects this route.
      const auto throttle = login_throttle().check(client.address, snapshot.username);
      if (!throttle.allowed) {
        web::CaseInsensitiveMultimap headers = json_headers();
        headers.emplace("Retry-After", std::to_string(throttle.retry_after.count()));
        nlohmann::json wait {
          { "status", false },
          { "error", "Too many attempts" },
          { "code", "rate_limited" },
          { "retry_after", throttle.retry_after.count() },
        };
        response->write(web::StatusCode::too_many_requests, wait.dump(), headers);
        return;
      }

      nlohmann::json body;
      if (!read_json_request(response, request, body)) return;

      std::string current_password;
      std::string new_password;
      std::string confirmation;
      std::string new_username;
      const bool has_new_username = body.contains("new_username");
      if (has_new_username && !read_string(body, "new_username", new_username)) {
        write_json_error(std::move(response), web::StatusCode::bad_request, "The new user name must be text");
        return;
      }
      // A user name change needs no new password: the current one, verified below, stays. A password change needs the new one.
      const bool renames = has_new_username && !new_username.empty() && new_username != snapshot.username;
      if (!read_string(body, "current_password", current_password) || (!renames && !read_string(body, "new_password", new_password))) {
        write_json_error(std::move(response), web::StatusCode::bad_request, "Current and new password are required");
        return;
      }
      if (renames) {
        read_string(body, "new_password", new_password);
        if (new_username.size() > maximum_username_length) {
          write_json_error(std::move(response), web::StatusCode::bad_request, "User name must be between 1 and 128 characters");
          return;
        }
      }
      if (renames && new_password.empty()) {
        // the password stays (it is verified below and saved again under the new name)
        new_password = current_password;
      }
      else if (new_password.size() < minimum_password_length || new_password.size() > maximum_password_length) {
        write_json_error(std::move(response), web::StatusCode::bad_request,
          "Password must be between 12 and 128 characters");
        return;
      }
      if (read_string(body, "confirm_password", confirmation) && confirmation != new_password) {
        write_json_error(std::move(response), web::StatusCode::bad_request, "Passwords do not match");
        return;
      }

      if (!verify_credentials(snapshot.username, current_password)) {
        login_throttle().record_failure(client.address, snapshot.username);
        BOOST_LOG(info) << "Web UI: ["sv << client.address << "] password change failed for ["sv
                        << mask_username(snapshot.username) << ']';
        write_json_error(std::move(response), web::StatusCode::unauthorized, "Invalid current password");
        return;
      }

      {
        std::lock_guard lock(credentials_mutex);
        if (!save_credentials_locked(renames ? new_username : snapshot.username, new_password)) {
          write_json_error(std::move(response), web::StatusCode::internal_server_error, "Failed to save credentials");
          return;
        }
      }

      login_throttle().record_success(client.address, snapshot.username);
      const auto token = get_cookie(request, session_cookie_name());
      const auto current_session = token ? session_store().lookup(*token, util::hex(crypto::hash(snapshot.password)).to_string()) : std::nullopt;

      // A credential change ends every other session. The initiating session
      // survives on a fresh identifier bound to the new credential, so a
      // token captured before the change is worthless: session identifiers
      // are renewed on sensitive state changes, not only at login
      // [ds5-owasp-asvs4-v3-20261103] V3.4.1.
      web::CaseInsensitiveMultimap headers = json_headers();
      nlohmann::json result { { "status", true } };
      if (token && current_session) {
        session_store().revoke_all(*token);
        auto rotated = session_store().create(
          snapshot.username,
          current_session->remember,
          client,
          credential_fingerprint(),
          std::optional<std::string_view> { *token });
        if (rotated) {
          add_session_cookies(headers, *rotated);
          result["session_id"] = rotated->public_id;
          result["csrf_token"] = rotated->csrf_token;
          result["expires_at"] = to_iso8601(rotated->expires_at);
          result["absolute_expires_at"] = to_iso8601(rotated->absolute_expires_at);
        }
        else {
          // The password changed but no fresh session could be issued. Fail
          // closed: clear the cookies so the client signs in again.
          add_cleared_session_cookies(headers);
        }
      }
      else {
        session_store().revoke_all();
      }
      response->write(web::StatusCode::ok, result.dump(), headers);
    }

    // POST /api/auth/password/reset: a new password without the old one, for the trusted local surface only (the desktop panel on this computer, through its loopback proxy).
    // The person who forgot the password cannot give the old one; whoever is on this computer already holds every loopback right (the same surface may add an app that runs a command).
    // The checks that guard every state change stay: authenticate() (Fetch Metadata and CSRF), loopback only, and no forwarding header, so a reverse proxy on this computer
    // cannot pass a remote request off as local. A reset ends every session and clears the login backoff.
    void
    handlePasswordReset(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;
      if (!require_localhost(response, request, "resetting the Web UI password")) return;
      if (request->header.find("x-forwarded-for") != request->header.end() ||
          request->header.find("forwarded") != request->header.end() ||
          request->header.find("x-real-ip") != request->header.end()) {
        write_json_error(std::move(response), web::StatusCode::forbidden, "A forwarded request cannot reset the password");
        return;
      }
      if (!check_content_type(response, request, "application/json")) return;

      const auto snapshot = snapshot_credentials();
      if (snapshot.username.empty()) {
        write_json_error(std::move(response), web::StatusCode::conflict, "No account exists yet");
        return;
      }

      nlohmann::json body;
      if (!read_json_request(response, request, body)) return;

      std::string new_password;
      std::string confirmation;
      if (!read_string(body, "new_password", new_password)) {
        write_json_error(std::move(response), web::StatusCode::bad_request, "A new password is required");
        return;
      }
      if (new_password.size() < minimum_password_length || new_password.size() > maximum_password_length) {
        write_json_error(std::move(response), web::StatusCode::bad_request,
          "Password must be between 12 and 128 characters");
        return;
      }
      if (read_string(body, "confirm_password", confirmation) && confirmation != new_password) {
        write_json_error(std::move(response), web::StatusCode::bad_request, "Passwords do not match");
        return;
      }

      {
        std::lock_guard lock(credentials_mutex);
        if (!save_credentials_locked(snapshot.username, new_password)) {
          write_json_error(std::move(response), web::StatusCode::internal_server_error, "Failed to save credentials");
          return;
        }
      }

      BOOST_LOG(info) << "Web UI: password reset from the local panel for ["sv << mask_username(snapshot.username) << ']';
      session_store().revoke_all();
      login_throttle().clear();
      response->write(web::StatusCode::ok, nlohmann::json { { "status", true } }.dump(), json_headers());
    }
  }  // namespace

  bool
  authorize_browser_request(resp_https_t response, req_https_t request) {
    // Sec-Fetch-Site semantics are defined by the Fetch Metadata specification
    // [lt3-w3c-fetch-metadata-20261107].
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

  bool
  authenticate(resp_https_t response, req_https_t request) {
    if (!authorize_browser_request(response, request)) return false;

    const auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    if (!address_allowed(request)) {
      BOOST_LOG(error) << "Web UI: ["sv << address << "] -- denied"sv;
      response->write(web::StatusCode::forbidden);
      return false;
    }

    const auto token = get_cookie(request, session_cookie_name());
    if (token && credentials_exist() && session_store().lookup(*token, credential_fingerprint())) {
      if (!csrf_allowed(request, token)) {
        BOOST_LOG(warning) << "Web UI: ["sv << address << "] CSRF token missing or invalid for ["sv
                           << request->path << "]: "sv << csrf_failure_reason(request);
        write_json_error(std::move(response), web::StatusCode::forbidden, "CSRF token missing or invalid");
        return false;
      }
      return true;
    }

    const auto auth = get_single_request_header(request, "authorization");
    if (auth.valid && auth.value) {
      constexpr auto basic_prefix = "Basic "sv;
      if (auth.value->size() > basic_prefix.size() && auth.value->substr(0, basic_prefix.size()) == basic_prefix) {
        std::string decoded;
        try {
          decoded = web::decode_base64(auth.value->substr(basic_prefix.size()));
        }
        catch (const std::exception &e) {
          BOOST_LOG(debug) << "Authentication: Base64 decode failed: " << e.what();
        }
        const auto separator = decoded.find(':');
        if (separator != std::string::npos && separator + 1 <= decoded.size()) {
          if (verify_credentials(decoded.substr(0, separator), decoded.substr(separator + 1))) {
            return true;
          }
        }
      }
    }

    const auto local_token = get_single_request_header(request, local_auth_token::header_name);
    if (is_local(request) && local_token.valid && local_token.value &&
        local_auth_token::matches(*local_token.value)) {
      return true;
    }

    send_unauthenticated(std::move(response), std::move(request));
    return false;
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

  void
  send_redirect(resp_https_t response, req_https_t request, const char *path) {
    const auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    BOOST_LOG(debug) << "Web UI: ["sv << address << "] -- redirect to ["sv << path << ']';
    const web::CaseInsensitiveMultimap headers {
      { "Location", path },
      { "Cache-Control", "no-store" },
    };
    response->write(web::StatusCode::temporary_redirect, headers);
  }

  void
  reset_session_state() {
    session_store().clear(false);
    login_throttle().clear();
  }

  namespace auth {
    void
    register_routes(https_server_t &server) {
      server.resource["^/login/?$"]["GET"] = send_login_page;
      server.resource["^/api/logout$"]["GET"] = handleLogout;
      server.resource["^/api/logout$"]["POST"] = handleLogout;
      server.resource["^/api/password$"]["POST"] = savePassword;

      server.resource["^/api/auth/login$"]["POST"] = handleLogin;
      server.resource["^/api/auth/logout$"]["POST"] = handleSessionLogout;
      server.resource["^/api/auth/session$"]["GET"] = handleSessionInfo;
      server.resource["^/api/auth/sessions$"]["GET"] = handleSessionList;
      server.resource["^/api/auth/sessions$"]["DELETE"] = handleSessionDelete;
      server.resource["^/api/auth/sessions/([A-Za-z0-9_-]+)$"]["DELETE"] = handleSessionDelete;
      server.resource["^/api/auth/setup$"]["POST"] = handleSetup;
      server.resource["^/api/auth/password$"]["POST"] = handlePasswordChange;
      server.resource["^/api/auth/password/reset$"]["POST"] = handlePasswordReset;
    }
  }  // namespace auth
}  // namespace confighttp
