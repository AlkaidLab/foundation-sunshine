/**
 * @file tests/unit/test_session_auth_routes.cpp
 * @brief End-to-end auth routes over a real TLS listener.
 */

#include "src/config.h"
#include "src/confighttp/auth.h"
#include "src/confighttp/local_auth_token.h"
#include "src/crypto.h"
#include "src/file_handler.h"
#include "src/http/client.h"
#include "src/httpcommon.h"
#include "src/logging.h"
#include "src/utility.h"

#include <array>
#include <atomic>
#include <boost/asio/ssl.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>

#ifdef _WIN32
  #include "src/platform/windows/misc.h"

// The local credential code asks the platform who it runs as.  This test links
// only the HTTP and auth sources, so it answers as a normal user process: the
// token file is then scoped to this process's own SID.
namespace platf {
  bool
  is_running_as_system() {
    return false;
  }

  HANDLE
  retrieve_users_token(bool) {
    return nullptr;
  }
}  // namespace platf
#endif

// Logging globals normally live in logging.cpp. The focused suite only needs
// the sink-compatible objects so the request handlers' BOOST_LOG calls work.
boost::log::sources::severity_logger<int> verbose(0);
boost::log::sources::severity_logger<int> debug(1);
boost::log::sources::severity_logger<int> info(2);
boost::log::sources::severity_logger<int> warning(3);
boost::log::sources::severity_logger<int> error(4);
boost::log::sources::severity_logger<int> fatal(5);

namespace config {
  sunshine_t sunshine;
}

namespace net {
  using namespace std::literals;

  net_e
  from_address(const std::string_view &view) {
    if (view == "127.0.0.1"sv || view == "::1"sv) {
      return PC;
    }
    if (view.starts_with("10."sv) || view.starts_with("172.16."sv)) {
      return LAN;
    }
    return WAN;
  }

  std::string
  addr_to_normalized_string(boost::asio::ip::address address) {
    return address.to_string();
  }
}  // namespace net

namespace http {
  net::net_e origin_web_ui_allowed = net::WAN;
  bool test_credentials_exist = true;

  int
  save_user_creds(const std::string &, const std::string &username, const std::string &password, bool) {
    config::sunshine.username = username;
    config::sunshine.salt = "route-test-salt";
    config::sunshine.password = util::hex(crypto::hash(password + config::sunshine.salt)).to_string();
    test_credentials_exist = true;
    return 0;
  }

  int
  reload_user_creds(const std::string &) {
    return 0;
  }

  bool
  user_creds_exist(const std::string &) {
    return test_credentials_exist;
  }
}  // namespace http

namespace file_handler {
  std::filesystem::path
  path_from_utf8(const std::string_view path) {
    return std::filesystem::path { std::string { path } };
  }
}  // namespace file_handler

namespace {
  using namespace std::literals;
  using json = nlohmann::json;

  struct cookies_t {
    std::string session;
    std::string csrf;
  };

  std::string
  cookie_name(const std::string_view base) {
    const auto port = config::sunshine.port == 0 ? 47989 : config::sunshine.port;
    auto name = std::string { "__Host-sunshine_" } + std::string { base };
    if (port != 47989) {
      name.append("_");
      name.append(std::to_string(port));
    }
    return name;
  }

  std::string
  cookie_line(const web::ClientResponse &response, const std::string_view name) {
    const auto range = response.header.equal_range("Set-Cookie");
    for (auto header = range.first; header != range.second; ++header) {
      if (header->second.starts_with(name) && header->second.size() > name.size() && header->second[name.size()] == '=') {
        return header->second;
      }
    }
    return {};
  }

  cookies_t
  cookies_from(const web::ClientResponse &response) {
    const auto value_from = [](const std::string &line) {
      const auto end = line.find(';');
      const auto pair = line.substr(0, end);
      const auto equals = pair.find('=');
      return equals == std::string::npos ? std::string {} : pair.substr(equals + 1);
    };
    return {
      .session = value_from(cookie_line(response, cookie_name("session"))),
      .csrf = value_from(cookie_line(response, cookie_name("csrf"))),
    };
  }

  std::string
  cookie_header(const cookies_t &cookies) {
    return cookie_name("session") + "=" + cookies.session + "; " + cookie_name("csrf") + "=" + cookies.csrf;
  }

  class SessionAuthRoutes: public testing::Test {
  protected:
    std::filesystem::path directory;
    std::unique_ptr<confighttp::https_server_t> server;
    std::thread listener;
    unsigned short bound_port = 0;

    std::string previous_username;
    std::string previous_password;
    std::string previous_salt;
    std::string previous_credentials_file;
    int previous_idle = 0;
    int previous_absolute = 0;
    int previous_remember_idle = 0;
    int previous_remember_days = 0;
    std::uint16_t previous_port = 0;
    std::string previous_config_file;
    net::net_e previous_origin = net::WAN;

    void
    SetUp() override {
      previous_username = config::sunshine.username;
      previous_password = config::sunshine.password;
      previous_salt = config::sunshine.salt;
      previous_credentials_file = config::sunshine.credentials_file;
      previous_idle = config::sunshine.session_idle_minutes;
      previous_absolute = config::sunshine.session_absolute_hours;
      previous_remember_idle = config::sunshine.session_remember_idle_minutes;
      previous_remember_days = config::sunshine.session_remember_days;
      previous_port = config::sunshine.port;
      previous_config_file = config::sunshine.config_file;
      previous_origin = http::origin_web_ui_allowed;

      directory = std::filesystem::temp_directory_path() /
                  ("sunshine-session-auth-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
      std::filesystem::create_directories(directory);

      config::sunshine.username = "route-user";
      config::sunshine.salt = "route-test-salt";
      config::sunshine.password = util::hex(crypto::hash("route-password" + config::sunshine.salt)).to_string();
      config::sunshine.config_file = (directory / "sunshine.conf").string();
      config::sunshine.credentials_file = (directory / "credentials.json").string();
      config::sunshine.session_idle_minutes = 30;
      config::sunshine.session_absolute_hours = 12;
      config::sunshine.session_remember_idle_minutes = 7 * 24 * 60;
      config::sunshine.session_remember_days = 30;
      config::sunshine.port = 47989;
      http::origin_web_ui_allowed = net::WAN;
      http::test_credentials_exist = true;
      confighttp::reset_session_state();
      ASSERT_TRUE(confighttp::local_auth_token::initialize());

      const auto credentials = crypto::gen_creds("localhost", 2048);
      std::ofstream(directory / "cert.pem") << credentials.x509;
      std::ofstream(directory / "key.pem") << credentials.pkey;
      server = std::make_unique<confighttp::https_server_t>((directory / "cert.pem").string(), (directory / "key.pem").string());
      confighttp::auth::register_routes(*server);

      server->resource["^/api/test-auth-required$"]["GET"] = [](auto response, auto request) {
        request->remote.address(boost::asio::ip::make_address("192.0.2.1"));
        if (confighttp::authenticate(response, request)) {
          response->write("authenticated");
        }
      };
      server->resource["^/api/test-local-auth-required$"]["GET"] = [](auto response, auto request) {
        if (confighttp::authenticate(response, request)) {
          response->write("authenticated");
        }
      };
      server->resource["^/page-test$"]["GET"] = [](auto response, auto request) {
        request->remote.address(boost::asio::ip::make_address("192.0.2.1"));
        if (confighttp::authenticate(response, request)) {
          response->write("page");
        }
      };
    }

    void
    TearDown() override {
      if (server) {
        server->stop();
      }
      if (listener.joinable()) {
        listener.join();
      }
      server.reset();
      confighttp::reset_session_state();

      config::sunshine.username = previous_username;
      config::sunshine.password = previous_password;
      config::sunshine.salt = previous_salt;
      config::sunshine.credentials_file = previous_credentials_file;
      config::sunshine.config_file = previous_config_file;
      config::sunshine.session_idle_minutes = previous_idle;
      config::sunshine.session_absolute_hours = previous_absolute;
      config::sunshine.session_remember_idle_minutes = previous_remember_idle;
      config::sunshine.session_remember_days = previous_remember_days;
      config::sunshine.port = previous_port;
      http::origin_web_ui_allowed = previous_origin;

      std::error_code ignored;
      std::filesystem::remove_all(directory, ignored);
    }

    std::unique_ptr<web::Client<web::HTTPS>>
    connect() {
      server->config.address = "127.0.0.1";
      server->config.port = 0;
      server->config.timeout_content = 5;
      auto ready = std::make_shared<std::promise<unsigned short>>();
      auto startup_signaled = std::make_shared<std::atomic_bool>(false);
      auto port = ready->get_future();
      listener = std::thread([this, ready, startup_signaled] {
        try {
          server->start([ready, startup_signaled](unsigned short bound_port) {
            startup_signaled->store(true);
            ready->set_value(bound_port);
          });
        }
        catch (...) {
          if (!startup_signaled->exchange(true)) {
            ready->set_exception(std::current_exception());
          }
        }
      });
      if (port.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
        throw std::runtime_error("test HTTPS listener did not start");
      }
      bound_port = port.get();
      auto client = std::make_unique<web::Client<web::HTTPS>>("127.0.0.1:" + std::to_string(bound_port), false);
      client->config.timeout = 5;
      client->config.timeout_connect = 5;
      return client;
    }

    std::shared_ptr<web::ClientResponse>
    login(web::Client<web::HTTPS> &client, const std::string &username, const std::string &password, bool remember = false) {
      const auto body = json { { "username", username }, { "password", password }, { "remember", remember } }.dump();
      return client.request("POST", "/api/auth/login", body, { { "Content-Type", "application/json" } });
    }

    std::string
    local_token() const {
      std::ifstream input(directory / "local-auth.token");
      return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
    }
  };
}  // namespace

TEST_F(SessionAuthRoutes, LoginSetsHostOnlyCookiesAndSessionEndpointReportsState) {
  auto client = connect();
  auto response = login(*client, "route-user", "route-password");
  ASSERT_EQ(response->status_code, "200 OK") << response->content.string();

  const auto cookies = cookies_from(*response);
  ASSERT_EQ(cookies.session.size(), 64u);
  ASSERT_EQ(cookies.csrf.size(), 64u);

  const auto session_line = cookie_line(*response, cookie_name("session"));
  EXPECT_NE(session_line.find("HttpOnly"), std::string::npos);
  EXPECT_NE(session_line.find("Secure"), std::string::npos);
  EXPECT_NE(session_line.find("SameSite=Strict"), std::string::npos);
  EXPECT_NE(session_line.find("Path=/"), std::string::npos);
  EXPECT_EQ(session_line.find("Domain="), std::string::npos);

  const auto csrf_line = cookie_line(*response, cookie_name("csrf"));
  EXPECT_EQ(csrf_line.find("HttpOnly"), std::string::npos);
  EXPECT_NE(csrf_line.find("Secure"), std::string::npos);

  const auto login_body = json::parse(response->content.string());
  const auto session = client->request("GET", "/api/auth/session", "", { { "Cookie", cookie_header(cookies) } });
  ASSERT_EQ(session->status_code, "200 OK");
  const auto body = json::parse(session->content.string());
  EXPECT_TRUE(body["authenticated"]);
  EXPECT_EQ(body["username"], "route-user");
  EXPECT_EQ(body["session_id"], login_body["session_id"]);
  // the page may read the names of this host's cookies from here (they carry the service port unless it is the default)
  EXPECT_EQ(body["session_cookie"], cookie_name("session"));
  EXPECT_EQ(body["csrf_cookie"], cookie_name("csrf"));
  EXPECT_TRUE(body.contains("created_at"));
  EXPECT_TRUE(body.contains("expires_at"));
  EXPECT_EQ(body.dump().find(cookies.session), std::string::npos);
}

TEST_F(SessionAuthRoutes, RememberMeSurvivesRestartAndRotatesThePersistentCookie) {
  auto client = connect();
  const auto response = login(*client, "route-user", "route-password", true);
  ASSERT_EQ(response->status_code, "200 OK");
  const auto cookies = cookies_from(*response);
  const auto old_body = json::parse(response->content.string());

  {
    std::ifstream persisted(directory / "web-ui-sessions.json");
    const std::string contents((std::istreambuf_iterator<char>(persisted)), std::istreambuf_iterator<char>());
    EXPECT_EQ(contents.find(cookies.session), std::string::npos);
    EXPECT_EQ(contents.find(cookies.csrf), std::string::npos);
  }

  // Resetting only the in-memory state models a host restart while preserving
  // the restricted durable record.
  confighttp::reset_session_state();
  const auto resumed = client->request("GET", "/api/auth/session", "", { { "Cookie", cookie_header(cookies) } });
  ASSERT_EQ(resumed->status_code, "200 OK") << resumed->content.string();
  const auto resumed_cookies = cookies_from(*resumed);
  const auto resumed_body = json::parse(resumed->content.string());
  EXPECT_TRUE(resumed_body["authenticated"]);
  EXPECT_TRUE(resumed_body["remember"]);
  EXPECT_NE(resumed_cookies.session, cookies.session);
  EXPECT_NE(resumed_body["session_id"], old_body["session_id"]);
  EXPECT_EQ(resumed->header.count("Set-Cookie"), 2u);

  const auto old_session = client->request("GET", "/api/auth/session", "", { { "Cookie", cookie_header(cookies) } });
  EXPECT_FALSE(json::parse(old_session->content.string())["authenticated"]);
}

TEST_F(SessionAuthRoutes, CookieNamesAreScopedToTheServicePort) {
  config::sunshine.port = 47989;
  auto client = connect();
  const auto first = login(*client, "route-user", "route-password", true);
  ASSERT_EQ(first->status_code, "200 OK");
  const auto first_cookies = cookies_from(*first);
  const auto first_session_name = cookie_name("session");
  EXPECT_NE(cookie_line(*first, first_session_name).find(first_session_name + "="), std::string::npos);

  config::sunshine.port = 47990;
  const auto second = login(*client, "route-user", "route-password", true);
  ASSERT_EQ(second->status_code, "200 OK");
  const auto second_cookies = cookies_from(*second);
  const auto second_session_name = cookie_name("session");
  EXPECT_NE(cookie_line(*second, second_session_name).find(second_session_name + "="), std::string::npos);
  EXPECT_NE(first_session_name, second_session_name);

  const auto first_cookie_header = first_session_name + "=" + first_cookies.session + "; " +
                                   "__Host-sunshine_csrf=" + first_cookies.csrf;
  const auto first_on_second_instance = client->request("GET", "/api/auth/session", "", { { "Cookie", first_cookie_header } });
  EXPECT_FALSE(json::parse(first_on_second_instance->content.string())["authenticated"]);

  const auto second_on_second_instance = client->request("GET", "/api/auth/session", "", { { "Cookie", cookie_header(second_cookies) } });
  EXPECT_TRUE(json::parse(second_on_second_instance->content.string())["authenticated"]);
}

TEST_F(SessionAuthRoutes, BrowserSessionDoesNotSurviveRestart) {
  auto client = connect();
  const auto response = login(*client, "route-user", "route-password");
  ASSERT_EQ(response->status_code, "200 OK");
  const auto cookies = cookies_from(*response);

  confighttp::reset_session_state();
  const auto after_restart = client->request("GET", "/api/auth/session", "", { { "Cookie", cookie_header(cookies) } });
  ASSERT_EQ(after_restart->status_code, "200 OK");
  EXPECT_FALSE(json::parse(after_restart->content.string())["authenticated"]);
}

TEST_F(SessionAuthRoutes, WrongUserAndWrongPasswordReturnTheSameBody) {
  auto client = connect();
  const auto wrong_user = login(*client, "nobody", "route-password");
  const auto wrong_password = login(*client, "route-user", "wrong-password");

  EXPECT_EQ(wrong_user->status_code, "401 Unauthorized");
  EXPECT_EQ(wrong_password->status_code, "401 Unauthorized");
  EXPECT_EQ(wrong_user->content.string(), wrong_password->content.string());
  EXPECT_EQ(wrong_user->header.count("WWW-Authenticate"), 0u);
  EXPECT_EQ(wrong_user->header.count("Set-Cookie"), 0u);
}

TEST_F(SessionAuthRoutes, NavigationRedirectsToLoginWhileApiReturnsJson) {
  auto client = connect();

  const auto api = client->request("GET", "/api/test-auth-required");
  EXPECT_EQ(api->status_code, "401 Unauthorized");
  const auto api_body = json::parse(api->content.string());
  EXPECT_EQ(api_body["code"], "unauthenticated");
  EXPECT_EQ(api->header.count("WWW-Authenticate"), 0u);

  const auto page = client->request("GET", "/page-test");
  EXPECT_EQ(page->status_code, "303 See Other");
  ASSERT_NE(page->header.find("Location"), page->header.end());
  EXPECT_EQ(page->header.find("Location")->second, "/login?next=%2Fpage-test");
}

TEST_F(SessionAuthRoutes, BasicCredentialsStillAuthenticateApiClientsWithoutAChallenge) {
  auto client = connect();
  const auto good = client->request("GET", "/api/test-auth-required", "",
    { { "Authorization", "Basic cm91dGUtdXNlcjpyb3V0ZS1wYXNzd29yZA==" } });
  EXPECT_EQ(good->status_code, "200 OK");
  EXPECT_EQ(good->content.string(), "authenticated");

  const auto bad = client->request("GET", "/api/test-auth-required", "",
    { { "Authorization", "Basic cm91dGUtdXNlcjp3cm9uZy1wYXNzd29yZA==" } });
  EXPECT_EQ(bad->status_code, "401 Unauthorized");
  EXPECT_EQ(bad->header.count("WWW-Authenticate"), 0u);
}

TEST_F(SessionAuthRoutes, LoopbackRequiresTheLocalTokenAndAcceptsItConstantTime) {
  auto client = connect();
  const auto without_token = client->request("GET", "/api/test-local-auth-required");
  EXPECT_EQ(without_token->status_code, "401 Unauthorized");

  const auto token = local_token();
  ASSERT_EQ(token.size(), 64u);
  const auto with_token = client->request("GET", "/api/test-local-auth-required", "",
    { { "X-Sunshine-Local-Token", token } });
  EXPECT_EQ(with_token->status_code, "200 OK");
  EXPECT_EQ(with_token->content.string(), "authenticated");

  const auto wrong_token = client->request("GET", "/api/test-local-auth-required", "",
    { { "X-Sunshine-Local-Token", std::string(64, '0') } });
  EXPECT_EQ(wrong_token->status_code, "401 Unauthorized");
}

TEST_F(SessionAuthRoutes, PasswordResetRequiresTheLocalTokenAndRevokesSessions) {
  auto client = connect();
  const auto login_response = login(*client, "route-user", "route-password");
  ASSERT_EQ(login_response->status_code, "200 OK");
  const auto cookies = cookies_from(*login_response);

  const auto body = json { { "new_password", "reset-route-password" } }.dump();
  const auto without_token = client->request("POST", "/api/auth/password/reset", body,
    { { "Content-Type", "application/json" } });
  EXPECT_EQ(without_token->status_code, "401 Unauthorized");

  const auto with_token = client->request("POST", "/api/auth/password/reset", body,
    { { "Content-Type", "application/json" },
      { "X-Sunshine-Local-Token", local_token() } });
  ASSERT_EQ(with_token->status_code, "200 OK") << with_token->content.string();
  EXPECT_TRUE(json::parse(with_token->content.string())["status"]);

  const auto old_session = client->request("GET", "/api/auth/session", "",
    { { "Cookie", cookie_header(cookies) } });
  EXPECT_FALSE(json::parse(old_session->content.string())["authenticated"]);
  EXPECT_EQ(login(*client, "route-user", "reset-route-password")->status_code, "200 OK");
}

TEST_F(SessionAuthRoutes, SessionProbeReportsSignedOutOnLoopbackWithoutCredentials) {
  auto client = connect();
  const auto response = client->request("GET", "/api/auth/session");
  ASSERT_EQ(response->status_code, "200 OK");
  const auto body = json::parse(response->content.string());
  EXPECT_FALSE(body["authenticated"]);
  EXPECT_FALSE(body["setup_required"]);
}

TEST_F(SessionAuthRoutes, LocalTokenRotationInvalidatesThePreviousValue) {
  const auto previous = local_token();
  ASSERT_TRUE(confighttp::local_auth_token::initialize());
  const auto current = local_token();
  ASSERT_EQ(current.size(), 64u);
  EXPECT_NE(current, previous);
  EXPECT_FALSE(confighttp::local_auth_token::matches(previous));
  EXPECT_TRUE(confighttp::local_auth_token::matches(current));
}

TEST_F(SessionAuthRoutes, SetupRunsOnlyWhileNoCredentialsExist) {
  auto client = connect();
  config::sunshine.username.clear();
  config::sunshine.password.clear();
  config::sunshine.salt.clear();
  http::test_credentials_exist = false;
  config::sunshine.credentials_file = (directory / "new-credentials.json").string();

  const auto short_password = client->request("POST", "/api/auth/setup",
    json { { "username", "new-user" }, { "password", "short" } }.dump(),
    { { "Content-Type", "application/json" } });
  EXPECT_EQ(short_password->status_code, "400 Bad Request");

  const auto setup = client->request("POST", "/api/auth/setup",
    json { { "username", "new-user" }, { "password", "twelve-chars!" } }.dump(),
    { { "Content-Type", "application/json" } });
  ASSERT_EQ(setup->status_code, "200 OK") << setup->content.string();
  EXPECT_EQ(config::sunshine.username, "new-user");
  EXPECT_FALSE(cookies_from(*setup).session.empty());

  const auto second = client->request("POST", "/api/auth/setup",
    json { { "username", "another" }, { "password", "twelve-chars!" } }.dump(),
    { { "Content-Type", "application/json" } });
  EXPECT_EQ(second->status_code, "409 Conflict");
}

TEST_F(SessionAuthRoutes, PasswordChangeRevokesOtherSessionsAndKeepsCurrent) {
  auto client = connect();
  const auto first = login(*client, "route-user", "route-password");
  const auto second = login(*client, "route-user", "route-password");
  ASSERT_EQ(first->status_code, "200 OK");
  ASSERT_EQ(second->status_code, "200 OK");
  const auto first_cookies = cookies_from(*first);
  const auto second_body = json::parse(second->content.string());

  const auto change = client->request("POST", "/api/auth/password",
    json { { "current_password", "route-password" }, { "new_password", "new-route-password" } }.dump(),
    { { "Content-Type", "application/json" },
      { "Cookie", cookie_header(first_cookies) },
      { "X-CSRF-Token", first_cookies.csrf } });
  ASSERT_EQ(change->status_code, "200 OK") << change->content.string();
  const auto change_body = json::parse(change->content.string());
  ASSERT_TRUE(change_body.contains("session_id"));
  const auto change_cookies = cookies_from(*change);
  EXPECT_FALSE(change_cookies.session.empty());
  EXPECT_NE(change_cookies.session, first_cookies.session);

  const auto listed = client->request("GET", "/api/auth/sessions", "", { { "Cookie", cookie_header(change_cookies) } });
  ASSERT_EQ(listed->status_code, "200 OK");
  const auto list_body = json::parse(listed->content.string());
  bool has_first = false;
  bool has_second = false;
  for (const auto &session : list_body["sessions"]) {
    has_first = has_first || session["id"] == change_body["session_id"];
    has_second = has_second || session["id"] == second_body["session_id"];
  }
  EXPECT_TRUE(has_first);
  EXPECT_FALSE(has_second);

  EXPECT_EQ(login(*client, "route-user", "route-password")->status_code, "401 Unauthorized");
  EXPECT_EQ(login(*client, "route-user", "new-route-password")->status_code, "200 OK");
}

TEST_F(SessionAuthRoutes, AuthenticatedNavigationToLoginDoesNotLoop) {
  auto client = connect();
  auto login_res = login(*client, "route-user", "route-password");
  ASSERT_EQ(login_res->status_code, "200 OK");
  const auto cookies = cookies_from(*login_res);

  // Authenticated user visits /login: must redirect to "/" or home, NOT loop back to "/login"
  const auto res1 = client->request("GET", "/login", "", { { "Cookie", cookie_header(cookies) } });
  EXPECT_EQ(res1->status_code, "303 See Other");
  ASSERT_NE(res1->header.find("Location"), res1->header.end());
  EXPECT_NE(res1->header.find("Location")->second, "/login");
  EXPECT_EQ(res1->header.find("Location")->second, "/");

  // Authenticated user visits /login?next=%2Fapps: must redirect to "/apps", NOT "/login?next=%2Fapps"
  const auto res2 = client->request("GET", "/login?next=%2Fapps", "", { { "Cookie", cookie_header(cookies) } });
  EXPECT_EQ(res2->status_code, "303 See Other");
  ASSERT_NE(res2->header.find("Location"), res2->header.end());
  EXPECT_EQ(res2->header.find("Location")->second, "/apps");
}

TEST_F(SessionAuthRoutes, PasswordChangeEnforcesRateLimiting) {
  auto client = connect();
  auto login_res = login(*client, "route-user", "route-password");
  ASSERT_EQ(login_res->status_code, "200 OK");
  const auto cookies = cookies_from(*login_res);

  for (int attempt = 0; attempt != 5; ++attempt) {
    auto res = client->request("POST", "/api/auth/password",
      json { { "current_password", "wrong-password" }, { "new_password", "new-route-password" } }.dump(),
      { { "Content-Type", "application/json" },
        { "Cookie", cookie_header(cookies) },
        { "X-CSRF-Token", cookies.csrf } });
    EXPECT_EQ(res->status_code, "401 Unauthorized");
  }

  // The 6th attempt should be blocked with 429 Too Many Requests
  auto res_blocked = client->request("POST", "/api/auth/password",
    json { { "current_password", "wrong-password" }, { "new_password", "new-route-password" } }.dump(),
    { { "Content-Type", "application/json" },
      { "Cookie", cookie_header(cookies) },
      { "X-CSRF-Token", cookies.csrf } });
  EXPECT_EQ(res_blocked->status_code, "429 Too Many Requests");
}

TEST_F(SessionAuthRoutes, PasswordChangeRotatesSessionIdentifier) {
  auto client = connect();
  auto login_res = login(*client, "route-user", "route-password");
  ASSERT_EQ(login_res->status_code, "200 OK");
  const auto original_cookies = cookies_from(*login_res);

  auto change = client->request("POST", "/api/auth/password",
    json { { "current_password", "route-password" }, { "new_password", "new-route-password" } }.dump(),
    { { "Content-Type", "application/json" },
      { "Cookie", cookie_header(original_cookies) },
      { "X-CSRF-Token", original_cookies.csrf } });
  ASSERT_EQ(change->status_code, "200 OK");

  // OWASP ASVS 4.0.3 V3.4.1 requires rotating the session token on password change
  const auto new_cookies = cookies_from(*change);
  EXPECT_FALSE(new_cookies.session.empty());
  EXPECT_NE(new_cookies.session, original_cookies.session);

  // The old session token must be revoked
  auto old_session_check = client->request("GET", "/api/auth/session", "", { { "Cookie", cookie_header(original_cookies) } });
  EXPECT_EQ(json::parse(old_session_check->content.string())["authenticated"], false);
}

TEST_F(SessionAuthRoutes, CookieAuthenticatedStateChangeRequiresCsrfHeader) {
  auto client = connect();
  const auto response = login(*client, "route-user", "route-password");
  ASSERT_EQ(response->status_code, "200 OK");
  const auto cookies = cookies_from(*response);

  const auto missing = client->request("POST", "/api/auth/logout", "", { { "Cookie", cookie_header(cookies) } });
  EXPECT_EQ(missing->status_code, "403 Forbidden");

  const auto wrong = client->request("POST", "/api/auth/logout", "",
    { { "Cookie", cookie_header(cookies) }, { "X-CSRF-Token", std::string(64, 'a') } });
  EXPECT_EQ(wrong->status_code, "403 Forbidden");

  const auto accepted = client->request("POST", "/api/auth/logout", "",
    { { "Cookie", cookie_header(cookies) }, { "X-CSRF-Token", cookies.csrf } });
  EXPECT_EQ(accepted->status_code, "200 OK");

  const auto listed = client->request("GET", "/api/auth/sessions", "", { { "Cookie", cookie_header(cookies) } });
  EXPECT_EQ(listed->status_code, "401 Unauthorized");
}

TEST_F(SessionAuthRoutes, FiveFailuresThenRateLimit) {
  auto client = connect();
  for (int attempt = 0; attempt != 5; ++attempt) {
    EXPECT_EQ(login(*client, "rate-limited-user", "wrong-password")->status_code, "401 Unauthorized");
  }
  const auto limited = login(*client, "rate-limited-user", "wrong-password");
  EXPECT_EQ(limited->status_code, "429 Too Many Requests");
  ASSERT_NE(limited->header.find("Retry-After"), limited->header.end());
  EXPECT_GT(std::stoi(limited->header.find("Retry-After")->second), 0);
}

TEST_F(SessionAuthRoutes, LoginRejectsCrossSiteBrowserRequests) {
  auto client = connect();
  const auto response = client->request("POST", "/api/auth/login",
    json { { "username", "route-user" }, { "password", "route-password" } }.dump(),
    { { "Content-Type", "application/json" }, { "Origin", "https://evil.example" } });
  EXPECT_EQ(response->status_code, "403 Forbidden");
}

TEST_F(SessionAuthRoutes, SessionListMasksAddressAndMarksCurrent) {
  auto client = connect();
  const auto first = login(*client, "route-user", "route-password");
  const auto second = login(*client, "route-user", "route-password");
  ASSERT_EQ(first->status_code, "200 OK");
  ASSERT_EQ(second->status_code, "200 OK");
  const auto cookies = cookies_from(*first);

  const auto listed = client->request("GET", "/api/auth/sessions", "", { { "Cookie", cookie_header(cookies) } });
  ASSERT_EQ(listed->status_code, "200 OK");
  const auto body = json::parse(listed->content.string());
  ASSERT_EQ(body["sessions"].size(), 2u);
  EXPECT_EQ(body["current"], json::parse(first->content.string())["session_id"]);
  for (const auto &session : body["sessions"]) {
    EXPECT_EQ(session["client_ip"], "127.0.0.x");
    EXPECT_LE(session["user_agent"].get<std::string>().size(), 99u);
  }
}
