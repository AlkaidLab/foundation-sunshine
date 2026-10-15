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
    start_https(const std::function<void(web::Server<web::HTTPS> &)> &configure = {}) {
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
      if (configure) configure(*server);
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

namespace {
  // Exercise the same listener, executor and SSE response used by both public
  // HTTPS endpoints. Only the upstream callback and resource limits are injected.
  class AiStreamTransport: public HttpSafety {
  protected:
    ai::http::StreamExecutor streams { 2 };
    std::chrono::milliseconds send_timeout { 5000 };
    int send_buffer_bytes = 0;
    using RawSocket = boost::asio::ssl::stream<tcp::socket>;

    unsigned short
    start(ai::http::StreamBackend backend = ai::processAiChatStream) {
      return start_https([this, backend = std::move(backend)](auto &listener) {
        listener.config.max_response_buffer_size = 32 * 1024;
        listener.config.timeout_stream = send_timeout;
        listener.config.send_buffer_bytes = send_buffer_bytes;
        listener.resource["^/stream$"]["POST"] = [this, backend](auto response, auto request) {
          ai::http::stream_response(response, request->content.string(), streams, backend, send_timeout);
        };
      });
    }
    void
    TearDown() override {
      // Drain blocking upstream jobs while network completions still run.
      streams.stop();
      HttpSafety::TearDown();
    }
    std::unique_ptr<web::Client<web::HTTPS>>
    client(unsigned short port) {
      auto result = std::make_unique<web::Client<web::HTTPS>>("127.0.0.1:" + std::to_string(port), false);
      result->config.timeout = 3;
      result->config.timeout_connect = 3;
      return result;
    }
    std::unique_ptr<RawSocket>
    raw(boost::asio::io_context &io, boost::asio::ssl::context &tls, unsigned short port, int receive_buffer_bytes = 0) {
      auto socket = std::make_unique<RawSocket>(io, tls);
      socket->set_verify_mode(boost::asio::ssl::verify_none);
      if (receive_buffer_bytes > 0) {
        socket->lowest_layer().open(tcp::v4());
        socket->lowest_layer().set_option(boost::asio::socket_base::receive_buffer_size(receive_buffer_bytes));
      }
      socket->lowest_layer().connect({ boost::asio::ip::make_address("127.0.0.1"), port });
      socket->handshake(boost::asio::ssl::stream_base::client);
      const std::string request = "POST /stream HTTP/1.1\r\nHost: localhost\r\nContent-Length: 15\r\n\r\n{\"stream\":true}";
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

TEST_F(AiStreamTransport, SplitsLargeBackendChunksWithoutLosingBinaryBytes) {
  std::string payload(65537, 'x');
  payload.front() = payload[16 * 1024 - 1] = payload.back() = '\0';
  auto delivered = std::make_shared<std::atomic_bool>(false);
  const auto port = start([payload, delivered](const auto &, auto sink, auto) {
    *delivered = sink(payload.data(), payload.size());
    return ai::AiProxyResult { *delivered ? 200 : 502, "", "text/event-stream" };
  });
  const auto wire = raw_stream(port);
  boost::beast::http::response_parser<boost::beast::http::string_body> parser;
  parser.eager(true);
  boost::system::error_code ec;
  const auto consumed = parser.put(boost::asio::buffer(wire), ec);
  EXPECT_TRUE(*delivered);
  ASSERT_FALSE(ec) << ec.message();
  ASSERT_TRUE(parser.is_done());
  EXPECT_EQ(consumed, wire.size());
  EXPECT_EQ(parser.get().result_int(), 200);
  EXPECT_TRUE(parser.get().chunked());
  EXPECT_EQ(parser.get().body(), payload);
  EXPECT_TRUE(wire.ends_with("0\r\n\r\n"));
}

TEST_F(AiStreamTransport, BoundedStreamsKeepOneIoThreadResponsiveAndReleaseCapacity) {
  Upstream upstream;
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
  EXPECT_EQ(client(port)->request("GET", "/ping")->content.string(), "pong");
  const auto rejected = client(port)->request("POST", "/stream", R"({"stream":true})");
  EXPECT_EQ(rejected->status_code, "503 Service Unavailable");
  ASSERT_NE(rejected->header.find("Retry-After"), rejected->header.end());
  EXPECT_EQ(rejected->header.find("Retry-After")->second, "1");
  EXPECT_EQ(upstream.accepted, 2);
  for (auto &socket : sockets) disconnect(*socket);
  ASSERT_TRUE(wait_until([&] { return upstream.closed == 2; }, 3s));
  ASSERT_TRUE(wait_until([&] { return streams.active() == 0; }));
  Upstream finite(true, true);
  const auto admitted = client(port)->request("POST", "/stream", R"({"stream":true})");
  EXPECT_EQ(admitted->status_code, "200 OK");
  EXPECT_EQ(admitted->content.string(), "data: hello\n\n");
  EXPECT_EQ(finite.accepted, 1);
  EXPECT_TRUE(wait_until([&] { return streams.active() == 0; }));
}

TEST_F(AiStreamTransport, SlowClientTimesOutWithoutQueuingUnboundedChunks) {
  send_timeout = 1000ms;
  send_buffer_bytes = 4096;
  struct State {
    std::atomic_bool aborted { false };
    std::atomic_int chunks { 0 };
  };
  auto state = std::make_shared<State>();
  const auto port = start([state](const auto &, auto sink, auto) {
    const std::string data(16 * 1024, 'x');
    for (; state->chunks < 1000; ++state->chunks) {
      if (!sink(data.data(), data.size())) {
        state->aborted = true;
        break;
      }
    }
    return ai::AiProxyResult { 502, "", "application/json" };
  });
  boost::asio::io_context io;
  boost::asio::ssl::context tls(boost::asio::ssl::context::tls_client);
  // Bound the receive window before connecting so OS buffer sizing cannot
  // postpone backpressure beyond the timeout assertion. Leave it unread.
  auto socket = raw(io, tls, port, 4096);
  EXPECT_TRUE(wait_until([&] { return state->aborted.load(); }, 5s));
  EXPECT_LT(state->chunks, 1000);
  EXPECT_TRUE(wait_until([&] { return streams.active() == 0; }));
}

TEST_F(AiStreamTransport, ShutdownCancelsIdleUpstreamAndJoinsWorkersBeforeStoppingListener) {
  Upstream upstream(false);
  const auto port = start();
  boost::asio::io_context io;
  boost::asio::ssl::context tls(boost::asio::ssl::context::tls_client);
  auto socket = raw(io, tls, port);
  ASSERT_TRUE(wait_until([&] { return upstream.accepted == 1; }));
  const auto before = std::chrono::steady_clock::now();
  streams.stop();
  EXPECT_LT(std::chrono::steady_clock::now() - before, 3s);
  EXPECT_EQ(streams.active(), 0);
  EXPECT_TRUE(wait_until([&] { return upstream.closed == 1; }));
  EXPECT_FALSE(streams.submit([] {}));
  EXPECT_EQ(client(port)->request("GET", "/ping")->content.string(), "pong");
  streams.stop();
  server->stop();
  if (listener.joinable()) listener.join();
}

TEST(AiStreamExecutor, RejectsInvalidCapacity) {
  EXPECT_THROW(ai::http::StreamExecutor(0), std::invalid_argument);
  EXPECT_THROW(ai::http::StreamExecutor(17), std::invalid_argument);
}

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
