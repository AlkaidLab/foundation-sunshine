/**
 * @file tests/tools/test_http_safety.cpp
 * @brief Real upstream/TLS regressions for image paths and streaming cancellation.
 */
#include "src/http/client.h"
#include "src/http/server.h"
#include <atomic>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <filesystem>
#include <fstream>
#include <future>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <optional>
#include <thread>

#include "src/ai/http_stream.h"
#include "src/confighttp/asset_paths.h"
#include "src/crypto.h"
#include "src/httpcommon.h"
#include "src/logging.h"
#ifdef SUNSHINE_AI_BEAST_PILOT_TESTS
  #include "src/ai/beast_server.h"
#endif
#ifdef _WIN32
  #include <windows.h>
  #include <winioctl.h>
#endif

// Isolate the real proxy transport from persisted credentials and global host
// configuration; every upstream in this suite is a local test server.
boost::log::sources::severity_logger<int> verbose, debug, info, warning, error, fatal;
namespace {
  std::string upstream_url;
}
namespace ai {
  nlohmann::json
  loadAiConfig() {
    return { { "enabled", true }, { "provider", "ollama" }, { "apiBase", upstream_url } };
  }
}  // namespace ai
namespace http {
  bool
  post_json(const std::string &, const std::string &, const std::map<std::string, std::string> &, std::string &, long &, long) {
    return false;
  }
}  // namespace http

namespace {
  using namespace std::chrono_literals;
  using tcp = boost::asio::ip::tcp;
  namespace fs = std::filesystem;

#ifdef _WIN32
  // Junction creation needs no symlink privilege; use it to exercise canonical
  // escape checks on Windows hosts where symbolic links are unavailable.
  bool
  create_junction(const fs::path &target, const fs::path &link) {
    fs::create_directory(link);
    HANDLE handle = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    const auto substitute = L"\\??\\" + fs::absolute(target).native();
    const auto display = fs::absolute(target).native();
    struct MountPoint {
      DWORD tag;
      WORD length, reserved;
      WORD substitute_offset, substitute_length, print_offset, print_length;
      wchar_t paths[4096];
    } data {};
    const auto characters = substitute.size() + display.size() + 2;
    if (characters > std::size(data.paths)) {
      CloseHandle(handle);
      return false;
    }
    data.tag = IO_REPARSE_TAG_MOUNT_POINT;
    data.length = static_cast<WORD>(8 + characters * sizeof(wchar_t));
    data.substitute_length = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    data.print_offset = static_cast<WORD>((substitute.size() + 1) * sizeof(wchar_t));
    data.print_length = static_cast<WORD>(display.size() * sizeof(wchar_t));
    std::copy(substitute.begin(), substitute.end(), data.paths);
    std::copy(display.begin(), display.end(), data.paths + substitute.size() + 1);
    DWORD returned;
    const bool success = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &data, 8 + data.length,
      nullptr, 0, &returned, nullptr);
    CloseHandle(handle);
    return success;
  }
#endif

  template <class Predicate>
  bool
  wait_until(Predicate ready, std::chrono::seconds timeout = 8s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!ready() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(10ms);
    return ready();
  }

  // Send one SSE event and then remain silent. Detect curl closing its socket
  // independently of its return value, while the upstream remains alive.
  class Upstream {
    struct Connection: std::enable_shared_from_this<Connection> {
      tcp::socket socket;
      boost::asio::streambuf request;
      std::array<char, 1024> buffer {};
      Upstream &owner;
      Connection(boost::asio::io_context &io, Upstream &owner): socket(io), owner(owner) {}
      void
      read_close() {
        auto self = shared_from_this();
        socket.async_read_some(boost::asio::buffer(buffer), [self](auto ec, auto) {
          if (ec)
            ++self->owner.closed;
          else
            self->read_close();
        });
      }
    };
    boost::asio::io_context io;
    tcp::acceptor acceptor { io, { tcp::v4(), 0 } };
    std::thread worker;
    bool finite;
    int response_code = 200;
    std::string response_body;
    bool interim_continue = false;
    std::optional<size_t> declared_length;
    void
    accept() {
      auto connection = std::make_shared<Connection>(io, *this);
      acceptor.async_accept(connection->socket, [this, connection](auto ec) {
        if (ec) return;
        accept();
        boost::asio::async_read_until(connection->socket, connection->request, "\r\n\r\n", [this, connection](auto ec, auto) {
          if (ec) return;
          ++accepted;
          auto data = std::make_shared<std::string>();
          if (interim_continue) *data += "HTTP/1.1 100 Continue\r\n\r\n";
          *data += "HTTP/1.1 " + web::status_code(static_cast<web::StatusCode>(response_code));
          *data += response_code == 200 ? "\r\nContent-Type: text/event-stream\r\n" : "\r\nContent-Type: application/json\r\n";
          *data += "Connection: close\r\n";
          if (finite) *data += "Content-Length: " + std::to_string(declared_length.value_or(response_body.size())) + "\r\n";
          *data += "\r\n" + response_body;
          boost::asio::async_write(connection->socket, boost::asio::buffer(*data), [this, connection, data](auto ec, auto) {
            if (!ec && finite) {
              boost::system::error_code ignored;
              connection->socket.close(ignored);
            }
            else if (!ec)
              connection->read_close();
          });
        });
      });
    }

  public:
    std::atomic_int accepted { 0 }, closed { 0 };
    void
    start() {
      upstream_url = "http://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port());
      accept();
      worker = std::thread([this] { io.run(); });
    }
    explicit Upstream(bool send_event = true, bool finite = false): finite(finite), response_body(send_event ? "data: hello\n\n" : "") {
      start();
    }
    Upstream(int status, std::string body, bool interim = false, std::optional<size_t> length = {}): finite(true), response_code(status), response_body(std::move(body)), interim_continue(interim), declared_length(length) {
      start();
    }
    ~Upstream() {
      io.stop();
      worker.join();
    }
  };

  class HttpSafety: public testing::Test {
  protected:
    fs::path directory;
    std::unique_ptr<web::Server<web::HTTPS>> server;
    std::thread listener;
    unsigned short
    start_https() {
      const auto credentials = crypto::gen_creds("localhost", 2048);
      std::ofstream(directory / "cert.pem") << credentials.x509;
      std::ofstream(directory / "key.pem") << credentials.pkey;
      server = std::make_unique<web::Server<web::HTTPS>>((directory / "cert.pem").string(), (directory / "key.pem").string());
      server->config.address = "127.0.0.1";
      server->config.port = 0;
      server->config.thread_pool_size = 1;
      server->resource["^/stream$"]["POST"] = [](auto response, auto request) {
        ai::http::stream_response(response, request->content.string());
      };
      server->resource["^/ping$"]["GET"] = [](auto response, auto) { response->write("pong"); };
      auto ready = std::make_shared<std::promise<unsigned short>>();
      auto port = ready->get_future();
      listener = std::thread([this, ready] { server->start([ready](auto port) { ready->set_value(port); }); });
      if (port.wait_for(5s) != std::future_status::ready) throw std::runtime_error("HTTPS startup timed out");
      return port.get();
    }
    void
    SetUp() override {
      directory = fs::temp_directory_path() / ("sunshine-http-safety-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
      fs::create_directories(directory / "assets");
      fs::create_directories(directory / "covers/sub");
      fs::create_directories(directory / "covers-sibling");
      std::ofstream(directory / "assets/box.png") << "placeholder";
      std::ofstream(directory / "assets/logo.png") << "logo";
      std::ofstream(directory / "covers/sub/image.png") << "cover";
      std::ofstream(directory / "covers-sibling/private.png") << "secret";
    }
    void
    TearDown() override {
      if (server) server->stop();
      if (listener.joinable()) listener.join();
      server.reset();
      std::error_code ec;
      fs::remove_all(directory, ec);
    }
    auto
    resolve(const fs::path &request) {
      return confighttp::assets::resolve_boxart_path(directory / "assets", directory / "covers", request);
    }
    std::string
    raw_stream(unsigned short port) {
      boost::asio::io_context io;
      boost::asio::ssl::context tls(boost::asio::ssl::context::tls_client);
      boost::asio::ssl::stream<tcp::socket> socket(io, tls);
      socket.set_verify_mode(boost::asio::ssl::verify_none);
      socket.lowest_layer().connect({ boost::asio::ip::make_address("127.0.0.1"), port });
      socket.handshake(boost::asio::ssl::stream_base::client);
      const std::string request = "POST /stream HTTP/1.1\r\nHost: localhost\r\nContent-Length: 15\r\n\r\n{\"stream\":true}";
      boost::asio::write(socket, boost::asio::buffer(request));
      std::string wire;
      std::array<char, 4096> buffer {};
      boost::system::error_code ec;
      do {
        wire.append(buffer.data(), socket.read_some(boost::asio::buffer(buffer), ec));
      } while (!ec);
      return wire;
    }
  };
}  // namespace

TEST_F(HttpSafety, AllowsFlatAssetsAndNestedCovers) {
  EXPECT_EQ(resolve("logo.png"), fs::weakly_canonical(directory / "assets/logo.png"));
  EXPECT_EQ(resolve("sub/image.png"), fs::weakly_canonical(directory / "covers/sub/image.png"));
  EXPECT_FALSE(resolve("missing.png"));
  EXPECT_TRUE(confighttp::assets::is_child_path(directory / "covers/missing.png", directory / "covers"));
  EXPECT_FALSE(resolve("."));
  EXPECT_FALSE(resolve(""));
}

TEST_F(HttpSafety, RejectsTraversalRootedPathsAndAlternateStreams) {
  EXPECT_FALSE(resolve("../covers-sibling/private.png"));
  EXPECT_FALSE(resolve(directory / "covers-sibling/private.png"));
  EXPECT_FALSE(resolve("/private.png"));
  EXPECT_FALSE(resolve("C:/private.png"));
  EXPECT_FALSE(resolve("C:private.png"));
  EXPECT_FALSE(resolve("sub/image.png:private"));
  EXPECT_FALSE(resolve(fs::path(std::string("sub/image.png\0extra", 19))));
#ifdef _WIN32
  EXPECT_FALSE(resolve("\\\\127.0.0.1\\share\\private.png"));
  EXPECT_FALSE(resolve("\\private.png"));
  EXPECT_FALSE(resolve("..\\covers-sibling\\private.png"));
  EXPECT_FALSE(confighttp::assets::is_child_path("D:/private.png", "C:/covers"));
#endif
  EXPECT_FALSE(confighttp::assets::is_child_path(directory / "covers-sibling/private.png", directory / "covers"));
  EXPECT_FALSE(confighttp::assets::is_child_path({}, directory / "covers"));
}

TEST_F(HttpSafety, RejectsSymlinkEscapesAndAssetSubdirectories) {
  fs::create_directories(directory / "assets/sub");
  std::ofstream(directory / "assets/sub/hidden.png") << "hidden";
  EXPECT_FALSE(resolve("sub/hidden.png"));
  std::error_code ec;
  fs::create_directory_symlink(directory / "covers-sibling", directory / "covers/link", ec);
#ifdef _WIN32
  if (ec) {
    ASSERT_TRUE(create_junction(directory / "covers-sibling", directory / "covers/link"));
    ec.clear();
  }
#endif
  if (ec) GTEST_SKIP() << "Creating symlinks requires privileges: " << ec.message();
  EXPECT_FALSE(resolve("link/private.png"));
}

TEST(AiStreamSafety, RejectingChunkClosesUpstreamImmediately) {
  Upstream upstream;
  const auto result = ai::processAiChatStream(R"({"stream":true})", [](auto, auto) { return false; });
  EXPECT_EQ(result.httpCode, 502);
  EXPECT_TRUE(wait_until([&] { return upstream.closed == 1; }));
}

TEST(AiStreamSafety, SuccessfulStreamPreservesBytesAndStatus) {
  Upstream upstream(true, true);
  std::string received;
  const auto result = ai::processAiChatStream(R"({"stream":true})", [&](const char *data, size_t length) {
    received.append(data, length);
    return true;
  });
  EXPECT_EQ(result.httpCode, 200);
  EXPECT_EQ(received, "data: hello\n\n");
}

TEST(AiStreamSafety, UpstreamErrorsNeverInvokeTheChunkCallback) {
  const std::string body = R"({"error":{"message":"upstream rejected request"}})";
  for (const int code : { 401, 429, 503 }) {
    SCOPED_TRACE(code);
    Upstream upstream(code, body, true);
    bool called = false;
    const auto result = ai::processAiChatStream(R"({"stream":true})", [&](auto, auto) { called = true; return true; });
    EXPECT_FALSE(called);
    EXPECT_EQ(result.httpCode, code == 503 ? 502 : code);
    EXPECT_EQ(result.body, body);
    EXPECT_EQ(result.contentType, "application/json");
  }
}

TEST(AiStreamSafety, OversizedErrorBodiesAbortWithoutStartingSse) {
  Upstream upstream(401, std::string(1024 * 1024 + 1, 'x'));
  bool called = false;
  const auto result = ai::processAiChatStream(R"({"stream":true})", [&](auto, auto) { called = true; return true; });
  EXPECT_FALSE(called);
  EXPECT_EQ(result.httpCode, 502);
  EXPECT_EQ(nlohmann::json::parse(result.body)["error"]["type"], "upstream_error");
}

TEST_F(HttpSafety, UpstreamErrorsKeepTheirBodyWithoutAnSseResponse) {
  const auto port = start_https();
  const std::string body = R"({"error":{"message":"upstream rejected request"}})";
  for (const int code : { 401, 429, 503 }) {
    for (const auto &error_body : { body, std::string() }) {
      SCOPED_TRACE(code);
      Upstream upstream(code, error_body, true);
      web::Client<web::HTTPS> client("127.0.0.1:" + std::to_string(port), false);
      client.config.timeout = 3;
      const auto response = client.request("POST", "/stream", R"({"stream":true})");
      EXPECT_EQ(response->status_code, code == 401 ? "401 Unauthorized" : code == 429 ? "429 Too Many Requests" :
                                                                                        "502 Bad Gateway");
      EXPECT_EQ(response->content.string(), error_body);
      ASSERT_NE(response->header.find("Content-Type"), response->header.end());
      EXPECT_EQ(response->header.find("Content-Type")->second, "application/json");
      EXPECT_EQ(response->header.count("Transfer-Encoding"), 0);
      ASSERT_NE(response->header.find("Content-Length"), response->header.end());
      EXPECT_EQ(response->header.find("Content-Length")->second, std::to_string(error_body.size()));
    }
  }
}

TEST_F(HttpSafety, BeastFramingPreservesBinaryBytesAndTerminatesSuccessfulProductionStreams) {
  const auto port = start_https();
  for (const size_t length : { 1U, 15U, 16U * 1024, 48U * 1024 + 3 }) {
    SCOPED_TRACE(length);
    std::string body(length, 'x');
    body[length / 2] = '\0';
    Upstream upstream(200, body);
    const auto wire = raw_stream(port);
    EXPECT_TRUE(wire.ends_with("0\r\n\r\n"));
    boost::beast::http::response_parser<boost::beast::http::string_body> parser;
    parser.eager(true);
    boost::system::error_code ec;
    const auto consumed = parser.put(boost::asio::buffer(wire), ec);
    ASSERT_FALSE(ec) << ec.message();
    ASSERT_TRUE(parser.is_done());
    EXPECT_EQ(consumed, wire.size());
    EXPECT_EQ(parser.get().result_int(), 200);
    EXPECT_TRUE(parser.get().chunked());
    EXPECT_FALSE(parser.get().keep_alive());
    EXPECT_EQ(parser.get()[boost::beast::http::field::content_type], "text/event-stream");
    EXPECT_EQ(parser.get().body(), body);
  }
}

TEST_F(HttpSafety, EmptySuccessfulUpstreamUsesAnEmptyHttpResponse) {
  Upstream upstream(false, true);
  const auto port = start_https();
  web::Client<web::HTTPS> client("127.0.0.1:" + std::to_string(port), false);
  client.config.timeout = 3;
  const auto response = client.request("POST", "/stream", R"({"stream":true})");
  EXPECT_EQ(response->status_code, "200 OK");
  EXPECT_EQ(response->content.string(), "");
  EXPECT_EQ(response->header.count("Transfer-Encoding"), 0);
  ASSERT_NE(response->header.find("Content-Length"), response->header.end());
  EXPECT_EQ(response->header.find("Content-Length")->second, "0");
}

TEST_F(HttpSafety, TruncatedUpstreamDoesNotCompleteTheDownstreamChunkedMessage) {
  const std::string body = "data: partial\n\n";
  Upstream upstream(200, body, false, body.size() + 100);
  const auto wire = raw_stream(start_https());
  EXPECT_FALSE(wire.ends_with("0\r\n\r\n"));
  boost::beast::http::response_parser<boost::beast::http::string_body> parser;
  parser.eager(true);
  boost::system::error_code ec;
  parser.put(boost::asio::buffer(wire), ec);
  ASSERT_TRUE(!ec || ec == boost::beast::http::error::need_more) << ec.message();
  EXPECT_EQ(parser.get().result_int(), 200);
  EXPECT_EQ(parser.get().body(), body);
  EXPECT_FALSE(parser.is_done());
  parser.put_eof(ec);
  EXPECT_EQ(ec, boost::beast::http::error::partial_message);
}

TEST(AiStreamSafety, ChunkExceptionsDoNotEscapeCurl) {
  Upstream upstream;
  EXPECT_NO_THROW({
    const auto result = ai::processAiChatStream(R"({"stream":true})", [](const char *, size_t) -> bool { throw std::runtime_error("write failure"); });
    EXPECT_EQ(result.httpCode, 502);
  });
  EXPECT_TRUE(wait_until([&] { return upstream.closed == 1; }));
}

TEST(AiStreamSafety, CancelsWhileUpstreamIsSilent) {
  Upstream upstream(false);
  auto request = std::async(std::launch::async, [&] {
    return ai::processAiChatStream(R"({"stream":true})", [](auto, auto) { return true; }, [&] { return upstream.accepted > 0; });
  });
  const auto finished = request.wait_for(5s);
  // Never wait for the production 300s timeout on a failing regression.
  EXPECT_EQ(finished, std::future_status::ready);
  if (finished == std::future_status::ready) EXPECT_EQ(request.get().httpCode, 502);
  EXPECT_TRUE(wait_until([&] { return upstream.closed == 1; }));
}

TEST(AiStreamSafety, CancellationExceptionsDoNotEscapeCurl) {
  Upstream upstream(false);
  EXPECT_NO_THROW({
    const auto result = ai::processAiChatStream("{}", [](auto, auto) { return true; }, []() -> bool { throw 42; });
    EXPECT_EQ(result.httpCode, 502);
  });
}

TEST_F(HttpSafety, SingleHttpsThreadRemainsResponsiveAndDisconnectCancelsAllStreams) {
  Upstream upstream;
  const auto port = start_https();

  boost::asio::io_context io;
  boost::asio::ssl::context tls(boost::asio::ssl::context::tls_client);
  using socket_t = boost::asio::ssl::stream<tcp::socket>;
  std::vector<std::unique_ptr<socket_t>> clients;
  for (int i = 0; i < 4; ++i) {
    auto socket = std::make_unique<socket_t>(io, tls);
    socket->set_verify_mode(boost::asio::ssl::verify_none);
    socket->lowest_layer().connect({ boost::asio::ip::make_address("127.0.0.1"), port });
    socket->handshake(boost::asio::ssl::stream_base::client);
    const std::string request = "POST /stream HTTP/1.1\r\nHost: localhost\r\nContent-Length: 15\r\n\r\n{\"stream\":true}";
    boost::asio::write(*socket, boost::asio::buffer(request));
    boost::asio::streambuf response;
    boost::asio::read_until(*socket, response, "data: hello\n\n");
    clients.push_back(std::move(socket));
  }
  web::Client<web::HTTPS> client("127.0.0.1:" + std::to_string(port), false);
  client.config.timeout = 3;
  EXPECT_EQ(client.request("GET", "/ping")->content.string(), "pong");
  EXPECT_EQ(client.request("POST", "/stream", R"({"stream":true})")->status_code, "503 Service Unavailable");
  for (auto &socket : clients) {
    socket->lowest_layer().set_option(boost::asio::socket_base::linger(true, 0));
    boost::system::error_code ec;
    socket->lowest_layer().close(ec);
  }
  EXPECT_TRUE(wait_until([&] { return upstream.closed == 4; }));
  // The permits are released as the cancelled jobs finish.
  EXPECT_TRUE(wait_until([&] { return ai::http::submit_stream([] {}); }));
  EXPECT_EQ(client.request("POST", "/stream", "")->status_code, "400 Bad Request");
}

TEST_F(HttpSafety, ProductionStreamCancelsBeforeTheFirstUpstreamChunk) {
  Upstream upstream(false);
  const auto port = start_https();
  boost::asio::io_context io;
  boost::asio::ssl::context tls(boost::asio::ssl::context::tls_client);
  boost::asio::ssl::stream<tcp::socket> client(io, tls);
  client.set_verify_mode(boost::asio::ssl::verify_none);
  client.lowest_layer().connect({ boost::asio::ip::make_address("127.0.0.1"), port });
  client.handshake(boost::asio::ssl::stream_base::client);
  const std::string request = "POST /stream HTTP/1.1\r\nHost: localhost\r\nContent-Length: 15\r\n\r\n{\"stream\":true}";
  boost::asio::write(client, boost::asio::buffer(request));
  ASSERT_TRUE(wait_until([&] { return upstream.accepted == 1; }));
  client.lowest_layer().set_option(boost::asio::socket_base::linger(true, 0));
  client.lowest_layer().close();
  EXPECT_TRUE(wait_until([&] { return upstream.closed == 1; }, 3s));
}

TEST_F(HttpSafety, ProductionSseRejectsHttp10BeforeCallingUpstream) {
  Upstream upstream(true, true);
  const auto port = start_https();
  boost::asio::io_context io;
  boost::asio::ssl::context tls(boost::asio::ssl::context::tls_client);
  boost::asio::ssl::stream<tcp::socket> socket(io, tls);
  socket.set_verify_mode(boost::asio::ssl::verify_none);
  socket.lowest_layer().connect({ boost::asio::ip::make_address("127.0.0.1"), port });
  socket.handshake(boost::asio::ssl::stream_base::client);
  const std::string request = "POST /stream HTTP/1.0\r\nHost: localhost\r\nContent-Length: 15\r\n\r\n{\"stream\":true}";
  boost::asio::write(socket, boost::asio::buffer(request));
  boost::beast::flat_buffer buffer;
  boost::beast::http::response<boost::beast::http::string_body> response;
  boost::beast::http::read(socket, buffer, response);
  EXPECT_EQ(response.result_int(), 505);
  EXPECT_EQ(response.version(), 10);
  EXPECT_FALSE(response.keep_alive());
  EXPECT_EQ(response.count(boost::beast::http::field::transfer_encoding), 0);
  EXPECT_EQ(upstream.accepted, 0);
}

#ifdef SUNSHINE_AI_BEAST_PILOT_TESTS
namespace {
  class BeastAiPilot: public HttpSafety {
  protected:
    ai::beast_http::Options options;
    std::unique_ptr<ai::beast_http::Server> pilot;
    std::atomic_bool saw_client_certificate { false };
    static constexpr const char *path = "/api/ai/chat/completions";
    void
    SetUp() override {
      HttpSafety::SetUp();
      const auto credentials = crypto::gen_creds("localhost", 2048);
      std::ofstream(directory / "cert.pem") << credentials.x509;
      std::ofstream(directory / "key.pem") << credentials.pkey;
      options.certificate_file = (directory / "cert.pem").string();
      options.private_key_file = (directory / "key.pem").string();
      options.io_threads = 1;
    }
    void
    TearDown() override {
      if (pilot) pilot->stop();
      pilot.reset();
      HttpSafety::TearDown();
    }
    unsigned short
    start(ai::beast_http::Backend backend = ai::processAiChatStream) {
      pilot = std::make_unique<ai::beast_http::Server>(options, [this](const auto &request, const auto &peer) {
        saw_client_certificate = peer.certificate_sha256.size() == 64;
        return request[boost::beast::http::field::authorization] == "Bearer pilot-regression-token"; }, std::move(backend));
      return pilot->start();
    }
    std::unique_ptr<web::Client<web::HTTPS>>
    client(unsigned short port) {
      auto result = std::make_unique<web::Client<web::HTTPS>>("127.0.0.1:" + std::to_string(port), false);
      result->config.timeout = 3;
      result->config.timeout_connect = 3;
      return result;
    }
    web::CaseInsensitiveMultimap
    headers() {
      return { { "Authorization", "Bearer pilot-regression-token" }, { "Content-Type", "application/json" } };
    }
    using RawSocket = boost::asio::ssl::stream<tcp::socket>;
    std::unique_ptr<RawSocket>
    raw(boost::asio::io_context &io, boost::asio::ssl::context &tls, unsigned short port) {
      auto socket = std::make_unique<RawSocket>(io, tls);
      socket->set_verify_mode(boost::asio::ssl::verify_none);
      socket->lowest_layer().connect({ boost::asio::ip::make_address("127.0.0.1"), port });
      socket->handshake(boost::asio::ssl::stream_base::client);
      const std::string request = "POST /api/ai/chat/completions HTTP/1.1\r\nHost: localhost\r\n"
                                  "Authorization: Bearer pilot-regression-token\r\nContent-Type: application/json\r\n"
                                  "Content-Length: 15\r\n\r\n{\"stream\":true}";
      boost::asio::write(*socket, boost::asio::buffer(request));
      return socket;
    }
    void
    disconnect(RawSocket &socket) {
      socket.lowest_layer().set_option(boost::asio::socket_base::linger(true, 0));
      boost::system::error_code ignored;
      socket.lowest_layer().close(ignored);
    }
  };
}  // namespace

TEST_F(BeastAiPilot, MatchesProductionSuccessfulStreamAndUsesBeastChunkFraming) {
  Upstream upstream(true, true);
  const auto legacy_port = start_https();
  const auto beast_port = start();
  web::Client<web::HTTPS> legacy("127.0.0.1:" + std::to_string(legacy_port), false);
  legacy.config.timeout = 3;
  const auto before = legacy.request("POST", "/stream", R"({"stream":true})");
  const auto after = client(beast_port)->request("POST", path, R"({"stream":true})", headers());
  EXPECT_EQ(after->status_code, before->status_code);
  EXPECT_EQ(after->content.string(), before->content.string());
  ASSERT_NE(after->header.find("Transfer-Encoding"), after->header.end());
  EXPECT_EQ(after->header.find("Transfer-Encoding")->second, "chunked");
  EXPECT_EQ(after->header.find("Content-Type")->second, "text/event-stream");
}

TEST_F(BeastAiPilot, SplitsLargeBackendChunksWithoutLosingBinaryBytes) {
  std::string payload(65537, 'x');
  payload.front() = payload[16 * 1024 - 1] = payload.back() = '\0';
  std::atomic_bool delivered { false };
  const auto port = start([payload, &delivered](const auto &, auto sink, auto) {
    delivered = sink(payload.data(), payload.size());
    return ai::AiProxyResult { delivered ? 200 : 502, "", "text/event-stream" };
  });
  std::shared_ptr<web::ClientResponse> response;
  EXPECT_NO_THROW(response = client(port)->request("POST", path, R"({"stream":true})", headers()));
  EXPECT_TRUE(delivered);
  ASSERT_TRUE(response);
  EXPECT_EQ(response->status_code, "200 OK");
  EXPECT_EQ(response->content.string(), payload);
}

TEST_F(BeastAiPilot, MatchesProductionUpstreamErrorsAndEmptyBodies) {
  const auto legacy_port = start_https();
  const auto beast_port = start();
  for (const int status : { 401, 429, 503 }) {
    for (const auto &body : { std::string(R"({"error":"rejected"})"), std::string() }) {
      SCOPED_TRACE(status);
      Upstream upstream(status, body, true);
      web::Client<web::HTTPS> legacy("127.0.0.1:" + std::to_string(legacy_port), false);
      legacy.config.timeout = 3;
      const auto before = legacy.request("POST", "/stream", R"({"stream":true})");
      const auto after = client(beast_port)->request("POST", path, R"({"stream":true})", headers());
      EXPECT_EQ(after->status_code, before->status_code);
      EXPECT_EQ(after->content.string(), before->content.string());
      EXPECT_EQ(after->header.find("Content-Type")->second, "application/json");
    }
  }
}

TEST_F(BeastAiPilot, RequiresAuthorizationAndValidatesBeforeCallingUpstream) {
  Upstream upstream(true, true);
  const auto port = start();
  EXPECT_EQ(client(port)->request("POST", path, R"({"stream":true})")->status_code, "401 Unauthorized");
  auto duplicate = headers();
  duplicate.emplace("Authorization", "Bearer forged");
  EXPECT_EQ(client(port)->request("POST", path, R"({"stream":true})", duplicate)->status_code, "401 Unauthorized");
  EXPECT_EQ(client(port)->request("POST", path, "not-json", headers())->status_code, "400 Bad Request");
  EXPECT_EQ(client(port)->request("POST", path, "{}", headers())->status_code, "400 Bad Request");
  EXPECT_EQ(client(port)->request("GET", path, "", headers())->status_code, "405 Method Not Allowed");
  EXPECT_EQ(client(port)->request("OPTIONS", path)->status_code, "204 No Content");
  EXPECT_EQ(upstream.accepted, 0);
}

TEST_F(BeastAiPilot, BoundsRequestBodies) {
  options.body_limit = 32;
  Upstream upstream(true, true);
  const auto port = start();
  EXPECT_EQ(client(port)->request("POST", path, std::string(100, 'x'), headers())->status_code, "413 Payload Too Large");
  EXPECT_EQ(upstream.accepted, 0);
}

TEST_F(BeastAiPilot, CancelsDisconnectedClientBeforeFirstUpstreamChunk) {
  Upstream upstream(false);
  const auto port = start();
  boost::asio::io_context io;
  boost::asio::ssl::context tls(boost::asio::ssl::context::tls_client);
  auto socket = raw(io, tls, port);
  ASSERT_TRUE(wait_until([&] { return upstream.accepted == 1; }));
  const auto before = std::chrono::steady_clock::now();
  disconnect(*socket);
  EXPECT_TRUE(wait_until([&] { return upstream.closed == 1; }, 3s));
  EXPECT_LT(std::chrono::steady_clock::now() - before, 3s);
}

TEST_F(BeastAiPilot, BoundedStreamsKeepOneIoThreadResponsiveAndReleaseCapacity) {
  Upstream upstream;
  options.max_streams = 2;
  const auto port = start();
  boost::asio::io_context io;
  boost::asio::ssl::context tls(boost::asio::ssl::context::tls_client);
  std::vector<std::unique_ptr<RawSocket>> sockets;
  for (int i = 0; i < 2; ++i) {
    auto socket = raw(io, tls, port);
    boost::asio::streambuf response;
    boost::asio::read_until(*socket, response, "data: hello\n\n");
    sockets.push_back(std::move(socket));
  }
  EXPECT_EQ(client(port)->request("OPTIONS", path)->status_code, "204 No Content");
  EXPECT_EQ(client(port)->request("POST", path, R"({"stream":true})", headers())->status_code, "503 Service Unavailable");
  for (auto &socket : sockets) disconnect(*socket);
  ASSERT_TRUE(wait_until([&] { return upstream.closed == 2; }, 3s));
  EXPECT_TRUE(wait_until([&] { return pilot->active_streams() == 0; }));
}

TEST_F(BeastAiPilot, SlowClientTimesOutWithoutQueuingUnboundedChunks) {
  options.io_timeout = 1000ms;
  options.send_buffer_bytes = 4096;
  std::atomic_bool aborted { false };
  std::atomic_int chunks { 0 };
  const auto port = start([&](const auto &, auto sink, auto) {
    const std::string data(16 * 1024, 'x');
    for (; chunks < 1000; ++chunks) {
      if (!sink(data.data(), data.size())) {
        aborted = true;
        break;
      }
    }
    return ai::AiProxyResult { 502, "", "application/json" };
  });
  boost::asio::io_context io;
  boost::asio::ssl::context tls(boost::asio::ssl::context::tls_client);
  auto socket = raw(io, tls, port);
  // Leave the downstream unread; the TCP/TLS buffers eventually apply backpressure.
  EXPECT_TRUE(wait_until([&] { return aborted.load(); }, 5s));
  EXPECT_LT(chunks, 1000);
}

TEST_F(BeastAiPilot, ShutdownCancelsIdleUpstreamAndJoinsWorkers) {
  Upstream upstream(false);
  const auto port = start();
  boost::asio::io_context io;
  boost::asio::ssl::context tls(boost::asio::ssl::context::tls_client);
  auto socket = raw(io, tls, port);
  ASSERT_TRUE(wait_until([&] { return upstream.accepted == 1; }));
  const auto before = std::chrono::steady_clock::now();
  pilot->stop();
  EXPECT_LT(std::chrono::steady_clock::now() - before, 3s);
  EXPECT_TRUE(wait_until([&] { return upstream.closed == 1; }));
  pilot->stop();
}

TEST_F(BeastAiPilot, TlsCertificateAndClientIdentityAreVerified) {
  Upstream upstream(true, true);
  const auto credentials = crypto::gen_creds("pilot-client", 2048);
  std::ofstream(directory / "client.pem") << credentials.x509;
  std::ofstream(directory / "client-key.pem") << credentials.pkey;
  options.client_ca_file = (directory / "client.pem").string();
  const auto port = start();
  auto missing_cert = client(port);
  EXPECT_THROW(missing_cert->request("POST", path, R"({"stream":true})", headers()), boost::system::system_error);
  const auto other = crypto::gen_creds("untrusted-client", 2048);
  std::ofstream(directory / "other.pem") << other.x509;
  std::ofstream(directory / "other-key.pem") << other.pkey;
  web::Client<web::HTTPS> untrusted("127.0.0.1:" + std::to_string(port), false,
    (directory / "other.pem").string(), (directory / "other-key.pem").string());
  untrusted.config.timeout = 3;
  EXPECT_THROW(untrusted.request("POST", path, R"({"stream":true})", headers()), boost::system::system_error);
  web::Client<web::HTTPS> authenticated("localhost:" + std::to_string(port), true,
    (directory / "client.pem").string(), (directory / "client-key.pem").string(), (directory / "cert.pem").string());
  authenticated.config.timeout = 3;
  EXPECT_EQ(authenticated.request("POST", path, R"({"stream":true})", headers())->content.string(), "data: hello\n\n");
  EXPECT_TRUE(saw_client_certificate);
  EXPECT_EQ(upstream.accepted, 1);
}

TEST_F(BeastAiPilot, RejectsPublicBindAndMissingAuthorizationPolicy) {
  options.address = "0.0.0.0";
  EXPECT_THROW(start(), std::invalid_argument);
  options.address = "127.0.0.1";
  EXPECT_THROW(ai::beast_http::Server(options, {}, ai::processAiChatStream), std::invalid_argument);
  options.max_streams = 0;
  EXPECT_THROW(start(), std::invalid_argument);
  options.max_streams = 17;
  EXPECT_THROW(start(), std::invalid_argument);
}

TEST_F(BeastAiPilot, CertificatePolicyCanAuthorizeWithoutBearerCredentials) {
  Upstream upstream(true, true);
  const auto credentials = crypto::gen_creds("paired-pilot-client", 2048);
  std::ofstream(directory / "client.pem") << credentials.x509;
  std::ofstream(directory / "client-key.pem") << credentials.pkey;
  options.client_ca_file = (directory / "client.pem").string();
  auto certificate = crypto::x509(credentials.x509);
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned length = 0;
  ASSERT_EQ(X509_digest(certificate.get(), EVP_sha256(), digest, &length), 1);
  std::string fingerprint;
  constexpr char hex[] = "0123456789abcdef";
  for (unsigned i = 0; i < length; ++i) {
    fingerprint += hex[digest[i] >> 4];
    fingerprint += hex[digest[i] & 15];
  }
  pilot = std::make_unique<ai::beast_http::Server>(options, [fingerprint](const auto &, const auto &peer) { return peer.certificate_sha256 == fingerprint; }, ai::processAiChatStream);
  const auto port = pilot->start();
  web::Client<web::HTTPS> authenticated("localhost:" + std::to_string(port), true,
    (directory / "client.pem").string(), (directory / "client-key.pem").string(), (directory / "cert.pem").string());
  authenticated.config.timeout = 3;
  EXPECT_EQ(authenticated.request("POST", path, R"({"stream":true})", { { "Content-Type", "application/json" } })->content.string(), "data: hello\n\n");
  EXPECT_EQ(upstream.accepted, 1);
}

TEST_F(BeastAiPilot, RejectsUnsupportedVersionAndAmbiguousHttpFraming) {
  Upstream upstream(true, true);
  const auto port = start();
  boost::asio::io_context io;
  boost::asio::ssl::context tls(boost::asio::ssl::context::tls_client);
  for (const bool ambiguous : { false, true }) {
    RawSocket socket(io, tls);
    socket.set_verify_mode(boost::asio::ssl::verify_none);
    socket.lowest_layer().connect({ boost::asio::ip::make_address("127.0.0.1"), port });
    socket.handshake(boost::asio::ssl::stream_base::client);
    std::string request = std::string("POST /api/ai/chat/completions HTTP/") + (ambiguous ? "1.1" : "1.0") +
                          "\r\nHost: localhost\r\nAuthorization: Bearer pilot-regression-token\r\nContent-Type: application/json\r\nContent-Length: 15\r\n";
    if (ambiguous) request += "Content-Length: 16\r\n";
    request += "\r\n{\"stream\":true}";
    boost::asio::write(socket, boost::asio::buffer(request));
    std::string received;
    std::array<char, 1024> data {};
    boost::system::error_code ec;
    do {
      const auto bytes = socket.read_some(boost::asio::buffer(data), ec);
      received.append(data.data(), bytes);
    } while (!ec);
    boost::beast::http::response_parser<boost::beast::http::string_body> parser;
    parser.eager(true);
    boost::system::error_code parse_error;
    parser.put(boost::asio::buffer(received), parse_error);
    ASSERT_FALSE(parse_error) << parse_error.message();
    ASSERT_TRUE(parser.is_done());
    EXPECT_EQ(parser.get().result_int(), ambiguous ? 400 : 505);
    EXPECT_EQ(parser.get().version(), ambiguous ? 11 : 10);
    EXPECT_FALSE(parser.get().keep_alive());
  }
  EXPECT_EQ(upstream.accepted, 0);
}
#endif

// Shutdown is intentionally last: it closes admission for the process lifetime.
TEST(AiStreamShutdown, CancelsIdleJobsBeforeListenersAreDestroyed) {
  Upstream upstream(false);
  std::promise<ai::AiProxyResult> result;
  auto finished = result.get_future();
  ASSERT_TRUE(ai::http::submit_stream([&] {
    result.set_value(ai::processAiChatStream("{}", [](auto, auto) { return true; }, ai::http::stream_shutdown_requested));
  }));
  EXPECT_TRUE(wait_until([&] { return upstream.accepted == 1; }));
  ai::http::shutdown_streams();
  EXPECT_EQ(finished.wait_for(0s), std::future_status::ready);
  EXPECT_EQ(finished.get().httpCode, 502);
  EXPECT_TRUE(wait_until([&] { return upstream.closed == 1; }));
  EXPECT_FALSE(ai::http::submit_stream([] {}));
  // Both listener shutdown paths can safely call this concurrently/repeatedly.
  ai::http::shutdown_streams();
}
