/**
 * @file tests/unit/test_confighttp_routes.cpp
 * @brief Route inventory and HTTPS authorization regression tests.
 */
#include "src/http/client.h"
#include <array>
#include <atomic>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <set>
#include <thread>
#include <vector>

#include "src/config.h"
#include "src/confighttp/auth.h"
#include "src/confighttp/routes.h"
#include "src/crypto.h"
#include "src/file_handler.h"
#include "src/httpcommon.h"
#include "src/platform/common.h"
#include "src/utility.h"

namespace {
  class ConfigHttpRoutes: public testing::Test {
  protected:
    std::filesystem::path directory;
    std::unique_ptr<confighttp::https_server_t> server;
    std::thread listener;
    unsigned short bound_port = 0;
    std::string previous_username;

    void
    SetUp() override {
      previous_username = config::sunshine.username;
      config::sunshine.username = "route-regression-test";
      directory = std::filesystem::temp_directory_path() /
                  ("sunshine-confighttp-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
      std::filesystem::create_directories(directory);
      const auto credentials = crypto::gen_creds("localhost", 2048);
      std::ofstream(directory / "cert.pem") << credentials.x509;
      std::ofstream(directory / "key.pem") << credentials.pkey;
      server = std::make_unique<confighttp::https_server_t>((directory / "cert.pem").string(), (directory / "key.pem").string());
      confighttp::register_routes(*server);
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
      config::sunshine.username = previous_username;
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

    std::string
    raw_get(const std::string &path) {
      boost::asio::io_context io;
      boost::asio::ssl::context tls(boost::asio::ssl::context::tls_client);
      boost::asio::ssl::stream<boost::asio::ip::tcp::socket> socket(io, tls);
      socket.set_verify_mode(boost::asio::ssl::verify_none);
      socket.lowest_layer().connect({ boost::asio::ip::make_address("127.0.0.1"), bound_port });
      socket.handshake(boost::asio::ssl::stream_base::client);
      const std::string request = "GET " + path + " HTTP/1.1\r\nHost: localhost\r\n\r\n";
      boost::asio::write(socket, boost::asio::buffer(request));
      std::string response;
      std::array<char, 1024> buffer {};
      boost::system::error_code error;
      do {
        const auto size = socket.read_some(boost::asio::buffer(buffer), error);
        response.append(buffer.data(), size);
      } while (!error);
      return response;
    }
  };

  using route_inventory_t = std::set<std::pair<std::string, std::string>>;

  route_inventory_t
  inventory(const confighttp::https_server_t &server) {
    route_inventory_t result;
    for (const auto &[pattern, methods] : server.resource) {
      for (const auto &[method, handler] : methods) {
        EXPECT_TRUE(handler) << pattern.str << ' ' << method;
        result.emplace(pattern.str, method);
      }
    }
    return result;
  }
}  // namespace

TEST_F(ConfigHttpRoutes, PreservesThePreSplitRouteInventory) {
  std::ifstream file(std::filesystem::path(SUNSHINE_SOURCE_DIR) / "tests/fixtures/confighttp_routes.json");
  ASSERT_TRUE(file.is_open());
  const auto fixture = nlohmann::json::parse(file);
  route_inventory_t expected;
  for (const auto &entry : fixture) {
#ifndef _WIN32
    if (entry.value("windows", false)) {
      continue;
    }
#endif
    expected.emplace(entry.at("pattern").get<std::string>(), entry.at("method").get<std::string>());
  }
  EXPECT_EQ(inventory(*server), expected);
  ASSERT_EQ(server->default_resource.size(), 1);
  EXPECT_TRUE(server->default_resource.at("GET"));
}

TEST_F(ConfigHttpRoutes, RouteCompositionIsIdempotentAndCanBeCopiedToLoopback) {
  const auto first = inventory(*server);
  confighttp::register_routes(*server);
  EXPECT_EQ(inventory(*server), first);
  confighttp::https_server_t loopback((directory / "cert.pem").string(), (directory / "key.pem").string());
  loopback.resource = server->resource;
  loopback.default_resource = server->default_resource;
  EXPECT_EQ(inventory(loopback), first);
  EXPECT_EQ(loopback.default_resource.size(), server->default_resource.size());
}

TEST_F(ConfigHttpRoutes, LocaleRemainsPublicAndKeepsLegacyResponseShape) {
  config::sunshine.username.clear();
  auto client = connect();
  const auto response = client->request("GET", "/api/configLocale");
  EXPECT_EQ(response->status_code, "200 OK");
  const auto body = nlohmann::json::parse(response->content.string());
  EXPECT_EQ(body["status"], "true");
  EXPECT_EQ(body["locale"], config::sunshine.locale);
}

TEST_F(ConfigHttpRoutes, LocalAuthenticatedApiKeepsItsResponseShape) {
  auto client = connect();
  const auto response = client->request("GET", "/api/v1/file-mapping/mappings");
  EXPECT_EQ(response->status_code, "200 OK");
  const auto body = nlohmann::json::parse(response->content.string());
  EXPECT_EQ(body["ok"], true);
  EXPECT_TRUE(body["mappings"].is_array());
}

TEST_F(ConfigHttpRoutes, RejectsCrossSiteBrowserApiRequests) {
  auto client = connect();
  const auto response = client->request("GET", "/api/v1/file-mapping/mappings", "", { { "Origin", "https://evil.example" } });
  EXPECT_EQ(response->status_code, "403 Forbidden");
  EXPECT_EQ(nlohmann::json::parse(response->content.string())["error"], "Cross-site request denied");
}

TEST_F(ConfigHttpRoutes, RejectsDuplicateOriginHeaders) {
  auto client = connect();
  const auto response = client->request("GET", "/api/v1/file-mapping/mappings", "", { { "Origin", "https://evil.example" }, { "Origin", "https://localhost" } });
  EXPECT_EQ(response->status_code, "403 Forbidden");
}

TEST_F(ConfigHttpRoutes, RequiresJsonBeforeCallingMutationHandlers) {
  auto client = connect();
  const auto response = client->request("POST", "/api/ai/config", "{}");
  EXPECT_EQ(response->status_code, "400 Bad Request");
  EXPECT_EQ(nlohmann::json::parse(response->content.string())["error"], "Content type not provided");
  const auto wrong_type = client->request("POST", "/api/apps", "{}", { { "Content-Type", "text/plain" } });
  EXPECT_EQ(wrong_type->status_code, "400 Bad Request");
  EXPECT_EQ(nlohmann::json::parse(wrong_type->content.string())["error"], "Content type mismatch");
}

TEST_F(ConfigHttpRoutes, FirstRunPasswordStillRejectsCrossSiteSources) {
  config::sunshine.username.clear();
  auto client = connect();
  const auto response = client->request("POST", "/api/password", "{}", { { "Content-Type", "application/json" }, { "Origin", "https://evil.example" } });
  EXPECT_EQ(response->status_code, "403 Forbidden");
  const auto unauthenticated = client->request("GET", "/api/v1/file-mapping/mappings");
  EXPECT_EQ(unauthenticated->status_code, "307 Temporary Redirect");
  ASSERT_NE(unauthenticated->header.find("Location"), unauthenticated->header.end());
  EXPECT_EQ(unauthenticated->header.find("Location")->second, "/welcome");
}

TEST_F(ConfigHttpRoutes, AiPreflightRemainsAccessibleWithoutCredentials) {
  config::sunshine.username.clear();
  auto client = connect();
  const auto response = client->request("OPTIONS", "/api/ai/chat/completions");
  EXPECT_EQ(response->status_code, "204 No Content");
  ASSERT_NE(response->header.find("Access-Control-Allow-Methods"), response->header.end());
  EXPECT_EQ(response->header.find("Access-Control-Allow-Methods")->second, "GET, POST, OPTIONS");
}

TEST_F(ConfigHttpRoutes, AiStreamStillRejectsCrossSiteAndDuplicateOriginBeforeStartingSse) {
  auto client = connect();
  for (const auto &headers : {
         web::CaseInsensitiveMultimap { { "Content-Type", "application/json" }, { "Origin", "https://evil.example" } },
         web::CaseInsensitiveMultimap { { "Content-Type", "application/json" }, { "Origin", "https://localhost" }, { "Origin", "https://evil.example" } } }) {
    const auto response = client->request("POST", "/api/ai/chat/completions", R"({"stream":true})", headers);
    EXPECT_EQ(response->status_code, "403 Forbidden");
    EXPECT_EQ(nlohmann::json::parse(response->content.string())["error"], "Cross-site request denied");
    EXPECT_EQ(response->header.count("Transfer-Encoding"), 0);
  }
}

TEST_F(ConfigHttpRoutes, AiStreamStillRequiresInitialCredentialsAndJson) {
  config::sunshine.username.clear();
  auto client = connect();
  const auto response = client->request("POST", "/api/ai/chat/completions", R"({"stream":true})", { { "Content-Type", "application/json" } });
  EXPECT_EQ(response->status_code, "307 Temporary Redirect");
  ASSERT_NE(response->header.find("Location"), response->header.end());
  EXPECT_EQ(response->header.find("Location")->second, "/welcome");
  EXPECT_EQ(response->header.count("Transfer-Encoding"), 0);
  EXPECT_EQ(client->request("POST", "/api/ai/chat/completions", R"({"stream":true})")->status_code, "400 Bad Request");
}

TEST_F(ConfigHttpRoutes, LocalLogoutAndUnknownRouteKeepTheirStatusCodes) {
  auto client = connect();
  EXPECT_EQ(client->request("POST", "/api/logout")->status_code, "200 OK");
  // Preserve the nonstandard status while using a complete Beast message.
  const auto wire = raw_get("/does-not-exist");
  EXPECT_TRUE(wire.starts_with("HTTP/1.1 444 No Response\r\n"));
  boost::beast::http::response_parser<boost::beast::http::string_body> parser;
  parser.eager(true);
  boost::system::error_code ec;
  parser.put(boost::asio::buffer(wire), ec);
  ASSERT_FALSE(ec) << ec.message();
  EXPECT_TRUE(parser.is_done());
  EXPECT_EQ(parser.get().result_int(), 444);
  EXPECT_FALSE(parser.get().keep_alive());
}

TEST_F(ConfigHttpRoutes, EveryRegisteredRouteDispatchesItsMethodPathAndCapturesOverTls) {
  auto replace = [](std::string &value, const std::string &from, const std::string &to) {
    size_t pos = 0;
    while ((pos = value.find(from, pos)) != std::string::npos) {
      value.replace(pos, from.size(), to);
      pos += to.size();
    }
  };
  struct Case {
    std::string path, method, marker, capture;
  };
  std::vector<Case> cases;
  for (auto &[pattern, methods] : server->resource) {
    auto path = pattern.str.substr(1, pattern.str.size() - 2);
    replace(path, "([0-9]+)", "42");
    // The remaining captures are component and clipboard/file-mapping IDs.
    path = std::regex_replace(path, std::regex(R"(\([^)]*\))"), "id_1");
    replace(path, "\\/", "/");
    replace(path, "/?", "/");
    replace(path, ".+", "file.bin");
    std::smatch match;
    ASSERT_TRUE(std::regex_match(path, match, pattern.regex)) << pattern.str << " => " << path;
    for (auto &[method, handler] : methods) {
      const auto marker = pattern.str + ' ' + method;
      cases.push_back({ path, method, marker, match.size() > 1 ? match[1].str() : "" });
      // Verify transport dispatch without executing restart, device, upload or
      // other business mutations during a route compatibility inventory test.
      handler = [marker](auto response, auto request) {
        response->write(nlohmann::json { { "marker", marker }, { "query", request->query_string },
          { "capture", request->path_match.size() > 1 ? request->path_match[1].str() : "" } }
            .dump());
      };
    }
  }
  auto client = connect();
  for (const auto &entry : cases) {
    SCOPED_TRACE(entry.marker);
    const auto response = client->request(entry.method, entry.path + "?transport=beast");
    ASSERT_EQ(response->status_code, "200 OK");
    const auto body = nlohmann::json::parse(response->content.string());
    EXPECT_EQ(body["marker"], entry.marker);
    EXPECT_EQ(body["query"], "transport=beast");
    EXPECT_EQ(body["capture"], entry.capture);
  }
}

TEST_F(ConfigHttpRoutes, BasicCredentialsStillAuthenticateRemoteRequestsAndRejectMalformedBase64) {
  const auto old_salt = config::sunshine.salt;
  const auto old_password = config::sunshine.password;
  const auto old_origin = http::origin_web_ui_allowed;
  auto restore = util::fail_guard([&] {
    config::sunshine.salt = old_salt;
    config::sunshine.password = old_password;
    http::origin_web_ui_allowed = old_origin;
  });
  config::sunshine.salt = "beast-test-salt";
  config::sunshine.password = util::hex(crypto::hash("wire-password" + config::sunshine.salt)).to_string();
  http::origin_web_ui_allowed = net::WAN;
  server->resource["^/api/test-remote-auth$"]["GET"] = [](auto response, auto request) {
    // Simulate the remote address for business authentication while headers
    // and response framing still traverse a real TLS connection.
    request->remote.address(boost::asio::ip::make_address("192.0.2.1"));
    if (confighttp::authenticate(response, request)) response->write("authenticated");
  };
  auto client = connect();
  const std::string good = "Basic cm91dGUtcmVncmVzc2lvbi10ZXN0OndpcmUtcGFzc3dvcmQ=";
  EXPECT_EQ(client->request("GET", "/api/test-remote-auth", "", { { "Authorization", good } })->content.string(), "authenticated");
  for (const auto &bad : { "Basic !!!=", "Basic d3Jvbmc=", "Basic cm91dGUtcmVncmVzc2lvbi10ZXN0Ondyb25n", "Bearer token" }) {
    const auto response = client->request("GET", "/api/test-remote-auth", "", { { "Authorization", bad } });
    EXPECT_EQ(response->status_code, "401 Unauthorized");
    EXPECT_EQ(response->header.count("WWW-Authenticate"), 1);
  }
}

TEST_F(ConfigHttpRoutes, ChunkedCoverUploadPreservesDecodedBinaryAndRejectsCrossSiteBeforeWriting) {
  const auto key = directory.filename().string();
  const auto path = platf::appdata() / "covers" / (key + ".png");
  ASSERT_FALSE(std::filesystem::exists(path));
  auto cleanup = util::fail_guard([&] { std::error_code ec; std::filesystem::remove(path, ec); });
  auto client = connect();
  const auto body = nlohmann::json { { "key", key }, { "data", "AAEC/3RhaWw=" } }.dump();
  const auto denied = client->request("POST", "/api/covers/upload", body,
    { { "Content-Type", "application/json" }, { "Origin", "https://evil.example" } });
  EXPECT_EQ(denied->status_code, "403 Forbidden");
  EXPECT_FALSE(std::filesystem::exists(path));
  boost::asio::io_context io;
  boost::asio::ssl::context tls(boost::asio::ssl::context::tls_client);
  boost::asio::ssl::stream<boost::asio::ip::tcp::socket> socket(io, tls);
  socket.set_verify_mode(boost::asio::ssl::verify_none);
  socket.lowest_layer().connect({ boost::asio::ip::make_address("127.0.0.1"), bound_port });
  socket.handshake(boost::asio::ssl::stream_base::client);
  namespace wire = boost::beast::http;
  wire::request<wire::string_body> request { wire::verb::post, "/api/covers/upload", 11 };
  request.set(wire::field::host, "localhost");
  request.set(wire::field::content_type, "application/json");
  request.keep_alive(false);
  request.body() = body;
  request.chunked(true);
  wire::write(socket, request);
  boost::beast::flat_buffer buffer;
  wire::response<wire::string_body> response;
  wire::read(socket, buffer, response);
  ASSERT_EQ(response.result_int(), 200) << response.body();
  std::ifstream file(path, std::ios::binary);
  ASSERT_TRUE(file.is_open());
  const std::string decoded((std::istreambuf_iterator<char>(file)), {});
  EXPECT_EQ(decoded, std::string("\0\1\2\xfftail", 8));
}

TEST_F(ConfigHttpRoutes, InvalidBase64CoverReturnsBadRequestWithoutCreatingAFile) {
  const auto key = directory.filename().string();
  const auto path = platf::appdata() / "covers" / (key + ".png");
  ASSERT_FALSE(std::filesystem::exists(path));
  auto cleanup = util::fail_guard([&] { std::error_code ec; std::filesystem::remove(path, ec); });
  auto client = connect();
  for (const auto *data : { "!@#$", "A" }) {
    SCOPED_TRACE(data);
    const auto body = nlohmann::json { { "key", key }, { "data", data } }.dump();
    std::shared_ptr<web::Client<web::HTTPS>::Response> response;
    ASSERT_NO_THROW(response = client->request("POST", "/api/covers/upload", body, { { "Content-Type", "application/json" } }));
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status_code, "400 Bad Request");
    const auto json = nlohmann::json::parse(response->content.string());
    ASSERT_TRUE(json.contains("error"));
    EXPECT_FALSE(json["error"].get<std::string>().empty());
    EXPECT_FALSE(std::filesystem::exists(path));
  }
}

TEST_F(ConfigHttpRoutes, FullLogDownloadStreamsFilesBeyondTheResponseBufferLimit) {
  const auto path = directory / std::filesystem::path(u8"large-日志.log");
  constexpr std::uint64_t length = 65 * 1024 * 1024;
  {
    std::ofstream file(path, std::ios::binary);
    file << "begin";
  }
  std::filesystem::resize_file(path, length);
  // Keep a writer open: downloading the log must not deny active writers.
  std::ofstream active(path, std::ios::binary | std::ios::app);
  ASSERT_TRUE(active.is_open());
  active << "end";
  active.flush();
  const auto old_path = config::sunshine.log_file;
  auto restore = util::fail_guard([&] { config::sunshine.log_file = old_path; });
  config::sunshine.log_file = file_handler::path_to_utf8(path);
  server->config.max_response_buffer_size = 64 * 1024;
  auto client = connect();
  boost::asio::io_context io;
  boost::asio::ssl::context tls(boost::asio::ssl::context::tls_client);
  boost::asio::ssl::stream<boost::asio::ip::tcp::socket> socket(io, tls);
  socket.set_verify_mode(boost::asio::ssl::verify_none);
  socket.lowest_layer().connect({ boost::asio::ip::make_address("127.0.0.1"), bound_port });
  socket.handshake(boost::asio::ssl::stream_base::client);
  namespace wire = boost::beast::http;
  wire::request<wire::empty_body> request { wire::verb::get, "/api/logs", 11 };
  request.set(wire::field::host, "localhost");
  request.keep_alive(false);
  wire::write(socket, request);
  boost::beast::flat_buffer buffer;
  wire::response_parser<wire::file_body> response;
  response.body_limit(length + 3);
  web::error_code ec;
  response.get().body().open(file_handler::path_to_utf8(directory / "received.log").c_str(), boost::beast::file_mode::write, ec);
  ASSERT_FALSE(ec) << ec.message();
  wire::read(socket, buffer, response, ec);
  ASSERT_FALSE(ec) << ec.message();
  EXPECT_EQ(response.get().result_int(), 200);
  response.get().body().close();
  const auto output = directory / "received.log";
  ASSERT_EQ(std::filesystem::file_size(output), length + 3);
  std::ifstream file(output, std::ios::binary);
  std::string bytes(5, '\0');
  file.read(bytes.data(), bytes.size());
  EXPECT_EQ(bytes, "begin");
  file.seekg(length);
  bytes.resize(3);
  file.read(bytes.data(), bytes.size());
  EXPECT_EQ(bytes, "end");
}
