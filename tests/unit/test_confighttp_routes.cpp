/**
 * @file tests/unit/test_confighttp_routes.cpp
 * @brief Route inventory and HTTPS authorization regression tests.
 */
#include <Simple-Web-Server/client_https.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <set>
#include <thread>

#include "src/config.h"
#include "src/confighttp/routes.h"
#include "src/crypto.h"

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

    std::unique_ptr<SimpleWeb::Client<SimpleWeb::HTTPS>>
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
      auto client = std::make_unique<SimpleWeb::Client<SimpleWeb::HTTPS>>("127.0.0.1:" + std::to_string(bound_port), false);
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

TEST_F(ConfigHttpRoutes, LocalLogoutAndUnknownRouteKeepTheirStatusCodes) {
  auto client = connect();
  EXPECT_EQ(client->request("POST", "/api/logout")->status_code, "200 OK");
  // The legacy default writes only a status line and closes the socket, so
  // inspect the wire response rather than expecting a complete HTTP message.
  EXPECT_EQ(raw_get("/does-not-exist"), "HTTP/1.1 444 No Response\r\n");
}
