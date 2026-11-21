/**
 * @file tests/unit/test_asset_https_routes.cpp
 * @brief Web UI asset handlers exercised over HTTPS with their real authentication.
 */
#include "src/config.h"
#include "src/confighttp/assets.h"
#include "src/confighttp/asset_etag.h"
#include "src/confighttp/auth.h"
#include "src/crypto.h"
#include "src/file_handler.h"
#include "src/http/client.h"
#include "src/httpcommon.h"
#include "src/platform/common.h"
#include "src/utility.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <gtest/gtest.h>
#include <iterator>
#include <thread>

namespace fs = std::filesystem;

// Other globals and credential stubs are shared with test_session_auth_routes.cpp.
namespace http {
  extern bool test_credentials_exist;
}
namespace file_handler {
  std::string path_to_utf8(const fs::path &path) { return path.string(); }
  std::string read_file(const char *path) {
    std::ifstream input(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
  }
}
namespace platf {
  fs::path appdata() { return fs::path { SUNSHINE_ASSETS_DIR }; }
}

namespace {
  constexpr auto authorization = "Basic cm91dGUtdXNlcjpyb3V0ZS1wYXNzd29yZA==";
  const std::string gzip_abc {
    '\x1f', '\x8b', '\x08', '\x00', '\x00', '\x00', '\x00', '\x00',
    '\x02', '\x0a', '\x4b', '\x4c', '\x4a', '\x06', '\x00', '\xc2',
    '\x41', '\x24', '\x35', '\x03', '\x00', '\x00', '\x00',
  };

  std::string header(const web::ClientResponse &response, const std::string &name) {
    const auto it = response.header.find(name);
    return it == response.header.end() ? std::string {} : it->second;
  }

  void write_file(const fs::path &path, const std::string &value) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output || !output.write(value.data(), static_cast<std::streamsize>(value.size())))
      throw std::runtime_error("failed to write test asset");
  }

  class AssetHttpsRoutes: public testing::Test {
  protected:
    // SUNSHINE_ASSETS_DIR is overridden for this test target to a directory
    // underneath its CMake binary tree; no installed files are touched.
    const fs::path fixture = fs::path { SUNSHINE_ASSETS_DIR } / "web";
    const fs::path html = fixture / "index.html";
    const fs::path plain = fixture / "assets/locale/asset-route-test.json";
    const fs::path compressed = fs::path { plain.string() + ".gz" };
    const fs::path marker = fs::path { compressed.string() + ".sha256" };
    const fs::path hashed = fixture / "assets/generated/asset-route-test-Abc12345.js";
    const fs::path cert = fixture / "asset-route-test-cert.pem";
    const fs::path key = fixture / "asset-route-test-key.pem";
    std::unique_ptr<confighttp::https_server_t> server;
    std::thread listener;
    std::unique_ptr<web::Client<web::HTTPS>> client;
    std::string old_username, old_password, old_salt, old_credentials;
    net::net_e old_origin {};
    bool old_creds {};

    void SetUp() override {
      old_username = config::sunshine.username;
      old_password = config::sunshine.password;
      old_salt = config::sunshine.salt;
      old_credentials = config::sunshine.credentials_file;
      old_origin = http::origin_web_ui_allowed;
      old_creds = http::test_credentials_exist;
      config::sunshine.username = "route-user";
      config::sunshine.salt = "route-test-salt";
      config::sunshine.password = util::hex(crypto::hash("route-password" + config::sunshine.salt)).to_string();
      config::sunshine.credentials_file = (fixture / "unused-credentials.json").string();
      http::origin_web_ui_allowed = net::WAN;
      http::test_credentials_exist = true;
      confighttp::reset_session_state();
      fs::create_directories(plain.parent_path());
      fs::create_directories(hashed.parent_path());
      write_file(html, "<html>First version</html>");
      write_file(plain, "abc");
      write_file(compressed, gzip_abc);
      write_file(marker, confighttp::assets::asset_sidecar_marker("abc", gzip_abc));
      write_file(hashed, "console.log(1)");
      const auto credentials = crypto::gen_creds("localhost", 2048);
      write_file(cert, credentials.x509);
      write_file(key, credentials.pkey);
      server = std::make_unique<confighttp::https_server_t>(cert.string(), key.string());
      confighttp::assets::register_routes(*server);
      server->config.address = "127.0.0.1";
      server->config.port = 0;
      server->config.timeout_content = 5;
      auto ready = std::make_shared<std::promise<unsigned short>>();
      auto signaled = std::make_shared<std::atomic_bool>(false);
      auto port = ready->get_future();
      listener = std::thread([this, ready, signaled] {
        try {
          server->start([ready, signaled](unsigned short p) { signaled->store(true); ready->set_value(p); });
        } catch (...) {
          if (!signaled->exchange(true)) ready->set_exception(std::current_exception());
        }
      });
      if (port.wait_for(std::chrono::seconds(10)) != std::future_status::ready)
        throw std::runtime_error("test HTTPS listener did not start");
      client = std::make_unique<web::Client<web::HTTPS>>("127.0.0.1:" + std::to_string(port.get()), false);
      client->config.timeout = 5;
      client->config.timeout_connect = 5;
    }

    void TearDown() override {
      client.reset();
      if (server) server->stop();
      if (listener.joinable()) listener.join();
      server.reset();
      confighttp::reset_session_state();
      config::sunshine.username = old_username;
      config::sunshine.password = old_password;
      config::sunshine.salt = old_salt;
      config::sunshine.credentials_file = old_credentials;
      http::origin_web_ui_allowed = old_origin;
      http::test_credentials_exist = old_creds;
      for (const auto &file : { html, plain, compressed, marker, hashed, cert, key }) {
        std::error_code ignored;
        fs::remove(file, ignored);
      }
    }

    std::shared_ptr<web::ClientResponse> get(const std::string &path, web::CaseInsensitiveMultimap headers = {}) {
      return client->request("GET", path, "", headers);
    }
  };
}

TEST_F(AssetHttpsRoutes, ValidatesAuthenticatedHtmlAndNegotiatedAssets) {
  EXPECT_NE(get("/")->status_code, "200 OK");
  const web::CaseInsensitiveMultimap auth { { "Authorization", authorization } };
  const auto page = get("/", auth);
  ASSERT_EQ(page->status_code, "200 OK");
  EXPECT_EQ(page->content.string(), "<html>First version</html>");
  EXPECT_EQ(header(*page, "Cache-Control"), "private, no-cache");
  const auto html_etag = header(*page, "ETag");
  ASSERT_FALSE(html_etag.empty());
  const auto cached_page = get("/", { { "Authorization", authorization }, { "If-None-Match", "W/" + html_etag } });
  EXPECT_EQ(cached_page->status_code, "304 Not Modified");
  EXPECT_EQ(cached_page->header.count("Content-Length"), 0u);
  EXPECT_TRUE(cached_page->content.string().empty());
  EXPECT_EQ(header(*cached_page, "ETag"), html_etag);
  EXPECT_NE(get("/", { { "If-None-Match", html_etag } })->status_code, "304 Not Modified");
  write_file(html, "<html>Second version</html>");
  const auto changed_page = get("/", { { "Authorization", authorization }, { "If-None-Match", html_etag } });
  EXPECT_EQ(changed_page->status_code, "200 OK");
  EXPECT_NE(header(*changed_page, "ETag"), html_etag);

  constexpr auto asset = "/assets/locale/asset-route-test.json";
  const auto identity = get(asset, { { "Accept-Encoding", "identity" } });
  ASSERT_EQ(identity->status_code, "200 OK");
  EXPECT_EQ(identity->content.string(), "abc");
  EXPECT_TRUE(header(*identity, "Content-Encoding").empty());
  EXPECT_EQ(header(*identity, "Cache-Control"), "private, no-cache");
  EXPECT_EQ(header(*identity, "Vary"), "Accept-Encoding");
  const auto identity_etag = header(*identity, "ETag");
  ASSERT_FALSE(identity_etag.empty());
  const auto identity_cached = get(asset, {
    { "Accept-Encoding", "identity" }, { "If-None-Match", "\"other\"" },
    { "If-None-Match", "W/" + identity_etag }
  });
  EXPECT_EQ(identity_cached->status_code, "304 Not Modified");
  EXPECT_EQ(identity_cached->header.count("Content-Length"), 0u);
  EXPECT_TRUE(identity_cached->content.string().empty());
  EXPECT_EQ(header(*identity_cached, "ETag"), identity_etag);
  EXPECT_EQ(header(*identity_cached, "Cache-Control"), "private, no-cache");
  const auto gzip = get(asset, { { "Accept-Encoding", "gzip" } });
  ASSERT_EQ(gzip->status_code, "200 OK");
  EXPECT_EQ(gzip->content.string(), gzip_abc);
  EXPECT_EQ(header(*gzip, "Content-Encoding"), "gzip");
  const auto negotiated = get(asset, {
    { "Accept-Encoding", "br" }, { "Accept-Encoding", "GZip ; Q=0.7, identity;q=0.2" }
  });
  ASSERT_EQ(negotiated->status_code, "200 OK");
  EXPECT_EQ(header(*negotiated, "Content-Encoding"), "gzip");
  EXPECT_EQ(negotiated->content.string(), gzip_abc);
  const auto preferred_identity = get(asset, { { "Accept-Encoding", "gzip;q=0.2, identity;q=0.8" } });
  ASSERT_EQ(preferred_identity->status_code, "200 OK");
  EXPECT_TRUE(header(*preferred_identity, "Content-Encoding").empty());
  EXPECT_EQ(preferred_identity->content.string(), "abc");
  const auto gzip_etag = header(*gzip, "ETag");
  ASSERT_FALSE(gzip_etag.empty());
  EXPECT_NE(identity_etag, gzip_etag);
  const auto gzip_cached = get(asset, { { "Accept-Encoding", "gzip" }, { "If-None-Match", "\"other\", W/" + gzip_etag } });
  EXPECT_EQ(gzip_cached->status_code, "304 Not Modified");
  EXPECT_EQ(gzip_cached->header.count("Content-Length"), 0u);
  EXPECT_TRUE(gzip_cached->content.string().empty());
  EXPECT_EQ(header(*gzip_cached, "Vary"), "Accept-Encoding");
  EXPECT_EQ(header(*gzip_cached, "ETag"), gzip_etag);
  EXPECT_EQ(header(*gzip_cached, "Content-Encoding"), "gzip");
  EXPECT_EQ(get(asset, { { "If-None-Match", "*" } })->status_code, "304 Not Modified");
  EXPECT_EQ(get(asset, { { "Accept-Encoding", "gzip" }, { "If-None-Match", identity_etag } })->status_code, "200 OK");
  EXPECT_EQ(get(asset, { { "Accept-Encoding", "identity" }, { "If-None-Match", gzip_etag } })->status_code, "200 OK");
  EXPECT_EQ(get(asset, { { "If-None-Match", "\"unmatched\"" }, { "If-Modified-Since", "Wed, 31 Dec 2099 23:59:59 GMT" } })->status_code, "200 OK");
  EXPECT_EQ(get(asset, { { "Accept-Encoding", "gzip;q=0, identity;q=0" } })->status_code, "406 Not Acceptable");
  EXPECT_EQ(get(asset, { { "Accept-Encoding", "gzip;q=, identity;q=0" } })->status_code, "406 Not Acceptable");
  const auto recovered_coding = get(asset, { { "Accept-Encoding", "gzip;q==bad, gzip;q=0.6" } });
  ASSERT_EQ(recovered_coding->status_code, "200 OK");
  EXPECT_EQ(header(*recovered_coding, "Content-Encoding"), "gzip");

  write_file(plain, "abd");
  const auto stale = get(asset, { { "Accept-Encoding", "gzip" }, { "If-None-Match", gzip_etag } });
  EXPECT_EQ(stale->status_code, "200 OK");
  EXPECT_EQ(stale->content.string(), "abd");
  EXPECT_TRUE(header(*stale, "Content-Encoding").empty());
  EXPECT_EQ(get(asset, { { "Accept-Encoding", "gzip, identity;q=0" } })->status_code, "406 Not Acceptable");
  write_file(plain, "abc");
  fs::remove(marker);
  const auto unmarked = get(asset, { { "Accept-Encoding", "gzip" } });
  EXPECT_EQ(unmarked->status_code, "200 OK");
  EXPECT_EQ(unmarked->content.string(), "abc");
  EXPECT_TRUE(header(*unmarked, "Content-Encoding").empty());
  write_file(marker, confighttp::assets::asset_sidecar_marker("abc", gzip_abc));
  write_file(compressed, gzip_abc + "damaged");
  const auto damaged = get(asset, { { "Accept-Encoding", "gzip" } });
  EXPECT_EQ(damaged->status_code, "200 OK");
  EXPECT_EQ(damaged->content.string(), "abc");
  EXPECT_TRUE(header(*damaged, "Content-Encoding").empty());
  fs::remove(compressed);
  fs::remove(marker);
  const auto absent = get(asset, { { "Accept-Encoding", "gzip" } });
  EXPECT_EQ(absent->status_code, "200 OK");
  EXPECT_EQ(absent->content.string(), "abc");
  EXPECT_TRUE(header(*absent, "Content-Encoding").empty());
  const auto hashed_asset = get("/assets/generated/asset-route-test-Abc12345.js");
  EXPECT_EQ(hashed_asset->status_code, "200 OK");
  EXPECT_EQ(header(*hashed_asset, "Cache-Control"), "private, max-age=31536000, immutable");
  const auto missing = get("/assets/locale/not-present.json");
  EXPECT_EQ(missing->status_code, "404 Not Found");
  EXPECT_EQ(header(*missing, "Cache-Control"), "no-store");
}
