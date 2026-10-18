/**
 * @file tests/tools/test_beast_http.cpp
 * @brief Real HTTP/TLS regressions for the shared Beast application transport.
 */
#include "src/crypto.h"
#include "src/http/client.h"
#include "src/http/server.h"
#include "src/logging.h"
#include <atomic>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <filesystem>
#include <fstream>
#include <future>
#include <gtest/gtest.h>
#include <mutex>
#include <thread>

boost::log::sources::severity_logger<int> verbose, debug, info, warning, error, fatal;
namespace {
  namespace net = boost::asio;
  namespace beast = boost::beast;
  namespace http = beast::http;
  namespace ssl = net::ssl;
  using tcp = net::ip::tcp;
  using namespace std::chrono_literals;
  template <class Predicate>
  bool
  wait_until(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(10ms);
    return predicate();
  }
  class BeastHttp: public testing::Test {
  protected:
    std::filesystem::path directory;
    std::unique_ptr<web::Server<web::HTTP>> plain;
    std::unique_ptr<web::Server<web::HTTPS>> encrypted;
    std::unique_ptr<web::Server<web::PairedHTTPS>> paired;
    std::thread listener;
    net::io_context io;
    ssl::context context { ssl::context::tls_client };
    beast::flat_buffer buffer;
    void
    SetUp() override {
      directory = std::filesystem::temp_directory_path() / ("sunshine-beast-http-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
      std::filesystem::create_directories(directory);
      const auto server = crypto::gen_creds("localhost", 2048);
      std::ofstream(directory / "cert.pem") << server.x509;
      std::ofstream(directory / "key.pem") << server.pkey;
      context.set_verify_mode(ssl::verify_none);
    }
    void
    TearDown() override {
      if (plain) plain->stop();
      if (encrypted) encrypted->stop();
      if (paired) paired->stop();
      if (listener.joinable()) listener.join();
      plain.reset();
      encrypted.reset();
      paired.reset();
      std::error_code ec;
      std::filesystem::remove_all(directory, ec);
    }
    template <class Server>
    unsigned short
    start(Server &server, std::string address = "127.0.0.1") {
      server.config.address = std::move(address);
      server.config.port = 0;
      server.config.thread_pool_size = 2;
      server.config.timeout_request = 2;
      server.config.timeout_content = 2;
      auto ready = std::make_shared<std::promise<unsigned short>>();
      auto future = ready->get_future();
      listener = std::thread([&server, ready] {
        bool signaled = false;
        try {
          server.start([&](auto port) { signaled = true; ready->set_value(port); });
        }
        catch (...) {
          if (!signaled) ready->set_exception(std::current_exception());
        }
      });
      if (future.wait_for(5s) != std::future_status::ready) throw std::runtime_error("Beast test listener did not start");
      return future.get();
    }
    auto
    socket(unsigned short port) {
      auto socket = std::make_unique<ssl::stream<tcp::socket>>(io, context);
      socket->lowest_layer().connect({ net::ip::make_address("127.0.0.1"), port });
      socket->handshake(ssl::stream_base::client);
      return socket;
    }
    auto
    secure_server() {
      return std::make_unique<web::Server<web::HTTPS>>((directory / "cert.pem").string(), (directory / "key.pem").string());
    }
    template <class Stream>
    auto
    request(Stream &stream, std::string target = "/echo", std::string body = "", bool keep = false) {
      http::request<http::string_body> message { http::verb::post, target, 11 };
      message.set(http::field::host, "localhost");
      message.keep_alive(keep);
      message.body() = body;
      message.prepare_payload();
      http::write(stream, message);
      http::response<http::string_body> response;
      http::read(stream, buffer, response);
      return response;
    }
  };
}  // namespace

TEST_F(BeastHttp, PlainHttpRoutesAndQueryValuesUseTheSameApplicationInterface) {
  plain = std::make_unique<web::Server<web::HTTP>>();
  plain->resource["^/echo$"]["POST"] = [](auto response, auto request) {
    const auto query = request->parse_query_string();
    response->write(request->content.string() + ':' + query.find("a")->second, { { "Content-Type", "text/plain" } });
  };
  tcp::socket client(io);
  client.connect({ net::ip::make_address("127.0.0.1"), start(*plain) });
  const auto response = request(client, "/echo?a=hello+world&a=other", "payload");
  EXPECT_EQ(response.body(), "payload:hello world");
  EXPECT_EQ(response.result(), http::status::ok);
}

TEST_F(BeastHttp, TlsKeepAliveRetainsPipelinedRequestsAndBinaryBodies) {
  encrypted = secure_server();
  encrypted->resource["^/echo$"]["POST"] = [](auto response, auto request) { response->write(request->content.string()); };
  auto client = socket(start(*encrypted));
  const std::string payload("first\0payload", 13);
  http::request<http::string_body> first { http::verb::post, "/echo", 11 };
  first.set(http::field::host, "localhost");
  first.keep_alive(true);
  first.body() = payload;
  first.prepare_payload();
  http::request<http::string_body> second { http::verb::post, "/echo", 11 };
  second.set(http::field::host, "localhost");
  second.keep_alive(false);
  second.body() = "second";
  second.prepare_payload();
  http::write(*client, first);
  http::write(*client, second);
  http::response<http::string_body> one, two;
  http::read(*client, buffer, one);
  http::read(*client, buffer, two);
  EXPECT_TRUE(one.keep_alive());
  EXPECT_FALSE(two.keep_alive());
  EXPECT_EQ(one.body(), payload);
  EXPECT_EQ(two.body(), "second");
}

TEST_F(BeastHttp, BeastDecodesChunkedUploadsBeforeCallingTheRoute) {
  encrypted = secure_server();
  encrypted->resource["^/echo$"]["POST"] = [](auto response, auto request) { response->write(request->content.string()); };
  auto client = socket(start(*encrypted));
  const std::string raw = "POST /echo HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n2\r\nde\r\n0\r\n\r\n";
  net::write(*client, net::buffer(raw));
  http::response<http::string_body> response;
  http::read(*client, buffer, response);
  EXPECT_EQ(response.body(), "abcde");
}

TEST_F(BeastHttp, ContinueIsSentBeforeWaitingForTheRequestBody) {
  encrypted = secure_server();
  encrypted->resource["^/echo$"]["POST"] = [](auto response, auto request) { response->write(request->content.string()); };
  auto client = socket(start(*encrypted));
  const std::string header = "POST /echo HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\nExpect: 100-continue\r\nContent-Length: 3\r\n\r\n";
  net::write(*client, net::buffer(header));
  http::response<http::empty_body> interim;
  http::read(*client, buffer, interim);
  EXPECT_EQ(interim.result(), http::status::continue_);
  net::write(*client, net::buffer("abc", 3));
  http::response<http::string_body> response;
  http::read(*client, buffer, response);
  EXPECT_EQ(response.body(), "abc");
}

TEST_F(BeastHttp, AmbiguousFramingAndOversizedBodiesNeverReachBusinessHandlers) {
  encrypted = secure_server();
  encrypted->config.max_request_streambuf_size = 256;
  std::atomic_int calls { 0 };
  encrypted->resource["^/echo$"]["POST"] = [&](auto response, auto) { ++calls; response->write("called"); };
  const auto port = start(*encrypted);
  for (const auto &fields : { std::string("Content-Length: 3\r\nContent-Length: 4\r\n"), std::string("Transfer-Encoding: chunked\r\nContent-Length: 3\r\n"), std::string("Content-Length: 500\r\n") }) {
    auto client = socket(port);
    const std::string raw = "POST /echo HTTP/1.1\r\nHost: localhost\r\n" + fields + "\r\nabc";
    net::write(*client, net::buffer(raw));
    http::response<http::string_body> response;
    http::read(*client, buffer, response);
    EXPECT_EQ(response.result_int(), fields.find("500") != std::string::npos ? 413 : 400);
  }
  EXPECT_EQ(calls, 0);
}

TEST_F(BeastHttp, StreamingConnectionObservesDisconnectBeforeAnyResponseBytes) {
  encrypted = secure_server();
  std::promise<std::shared_ptr<web::Server<web::HTTPS>::Response>> held;
  auto future = held.get_future();
  encrypted->resource["^/echo$"]["POST"] = [&](auto response, auto) { response->begin_stream(); held.set_value(response); };
  auto client = socket(start(*encrypted));
  const std::string raw = "POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n";
  net::write(*client, net::buffer(raw));
  ASSERT_EQ(future.wait_for(3s), std::future_status::ready);
  auto response = future.get();
  client->lowest_layer().set_option(net::socket_base::linger(true, 0));
  client->lowest_layer().close();
  EXPECT_TRUE(wait_until([&] { return response->closed(); }));
  std::atomic_int callbacks { 0 };
  response->send([&](auto ec) { EXPECT_TRUE(ec); ++callbacks; });
  EXPECT_TRUE(wait_until([&] { return callbacks == 1; }));
}

TEST_F(BeastHttp, ResponseHeaderInjectionFailsBeforeWritingToTheConnection) {
  encrypted = secure_server();
  encrypted->resource["^/echo$"]["POST"] = [](auto response, auto) { response->write("body", { { "X-Value", "bad\r\nInjected: true" } }); };
  auto client = socket(start(*encrypted));
  const auto response = request(*client);
  EXPECT_EQ(response.result(), http::status::internal_server_error);
  EXPECT_EQ(response.count("Injected"), 0);
}

TEST_F(BeastHttp, StopCancelsDeferredStreamsAndCanBeRepeated) {
  encrypted = secure_server();
  std::promise<std::shared_ptr<web::Server<web::HTTPS>::Response>> held;
  auto future = held.get_future();
  std::weak_ptr<web::Request> session_request;
  encrypted->resource["^/echo$"]["POST"] = [&](auto response, auto request) { session_request = request; response->begin_stream(); held.set_value(response); };
  auto client = socket(start(*encrypted));
  const std::string raw = "POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n";
  net::write(*client, net::buffer(raw));
  ASSERT_EQ(future.wait_for(3s), std::future_status::ready);
  auto response = future.get();
  const auto before = std::chrono::steady_clock::now();
  encrypted->stop();
  listener.join();
  encrypted->stop();
  EXPECT_TRUE(response->closed());
  EXPECT_LT(std::chrono::steady_clock::now() - before, 3s);
  std::atomic_int callbacks { 0 };
  response->send([&](auto ec) { EXPECT_TRUE(ec); ++callbacks; });
  EXPECT_EQ(callbacks, 1);
  response.reset();
  EXPECT_TRUE(session_request.expired());
}

TEST_F(BeastHttp, PairedIdentityBelongsToTheTlsConnectionAcrossKeepAliveRequests) {
  const auto credentials = crypto::gen_creds("paired-client", 2048);
  std::ofstream(directory / "client.pem") << credentials.x509;
  std::ofstream(directory / "client-key.pem") << credentials.pkey;
  context.use_certificate_chain_file((directory / "client.pem").string());
  context.use_private_key_file((directory / "client-key.pem").string(), ssl::context::pem);
  paired = std::make_unique<web::Server<web::PairedHTTPS>>((directory / "cert.pem").string(), (directory / "key.pem").string());
  std::atomic_int verified { 0 };
  paired->identify = [](X509 *) { return "paired-uuid"; };
  paired->resource["^/echo$"]["POST"] = [](auto response, auto request) { response->write(request->client_uuid); };
  // The explicit paired verification policy authorizes this certificate;
  // it is not validated as a public CA certificate by the transport.
  paired->verify = [&](SSL *session) {
    ++verified;
    crypto::x509_t cert { SSL_get1_peer_certificate(session) };
    return cert && crypto::x509_matches_pem(cert.get(), credentials.x509);
  };
  auto client = socket(start(*paired));
  EXPECT_EQ(request(*client, "/echo", "", true).body(), "paired-uuid");
  EXPECT_EQ(request(*client, "/echo", "", false).body(), "paired-uuid");
  EXPECT_EQ(verified, 3);  // Handshake and each request use the current policy.
}

TEST_F(BeastHttp, RejectedPairedCertificateCannotReachTheApplicationRoute) {
  const auto credentials = crypto::gen_creds("unpaired-client", 2048);
  std::ofstream(directory / "client.pem") << credentials.x509;
  std::ofstream(directory / "client-key.pem") << credentials.pkey;
  context.use_certificate_chain_file((directory / "client.pem").string());
  context.use_private_key_file((directory / "client-key.pem").string(), ssl::context::pem);
  paired = std::make_unique<web::Server<web::PairedHTTPS>>((directory / "cert.pem").string(), (directory / "key.pem").string());
  std::atomic_int calls { 0 };
  paired->verify = [](SSL *) { return 0; };
  paired->identify = [](X509 *) { return ""; };
  paired->on_verify_failed = [](auto response, auto) { response->write(web::StatusCode::forbidden, "unpaired"); };
  paired->resource["^/echo$"]["POST"] = [&](auto response, auto) { ++calls; response->write("called"); };
  auto client = socket(start(*paired));
  // Denial is returned immediately after handshake, before a request is read.
  http::response<http::string_body> response;
  http::read(*client, buffer, response);
  EXPECT_EQ(response.result(), http::status::forbidden);
  EXPECT_EQ(calls, 0);
}

TEST_F(BeastHttp, PairedTransportCannotStartWithoutVerificationAndIdentityPolicies) {
  web::Server<web::PairedHTTPS> missing((directory / "cert.pem").string(), (directory / "key.pem").string());
  EXPECT_THROW(missing.start(), std::invalid_argument);
  missing.verify = [](SSL *) { return 1; };
  EXPECT_THROW(missing.start(), std::invalid_argument);
}

TEST_F(BeastHttp, CaClientCertificatePolicyAndServerHostnameVerificationWorkTogether) {
  const auto credentials = crypto::gen_creds("trusted-client", 2048);
  std::ofstream(directory / "client.pem") << credentials.x509;
  std::ofstream(directory / "client-key.pem") << credentials.pkey;
  context.load_verify_file((directory / "cert.pem").string());
  context.set_verify_mode(ssl::verify_peer);
  context.set_verify_callback(ssl::host_name_verification("localhost"));
  context.use_certificate_chain_file((directory / "client.pem").string());
  context.use_private_key_file((directory / "client-key.pem").string(), ssl::context::pem);
  encrypted = std::make_unique<web::Server<web::HTTPS>>((directory / "cert.pem").string(), (directory / "key.pem").string(), (directory / "client.pem").string());
  auto calls = std::make_shared<std::atomic_int>(0);
  encrypted->resource["^/echo$"]["POST"] = [calls](auto response, auto request) {
    ++*calls;
    EXPECT_EQ(request->certificate_sha256.size(), 64);
    response->write("trusted");
  };
  const auto port = start(*encrypted);
  auto client = socket(port);
  EXPECT_EQ(request(*client).body(), "trusted");
  ssl::context anonymous_context { ssl::context::tls_client };
  ssl::stream<tcp::socket> anonymous(io, anonymous_context);
  anonymous.lowest_layer().connect({ net::ip::make_address("127.0.0.1"), port });
  web::error_code ec;
  anonymous.handshake(ssl::stream_base::client, ec);
  if (!ec) {
    std::array<char, 1> byte;
    anonymous.read_some(net::buffer(byte), ec);
  }
  EXPECT_TRUE(ec);
  const auto other = crypto::gen_creds("untrusted-client", 2048);
  std::ofstream(directory / "other.pem") << other.x509;
  std::ofstream(directory / "other-key.pem") << other.pkey;
  web::Client<web::HTTPS> untrusted("127.0.0.1:" + std::to_string(port), false,
    (directory / "other.pem").string(), (directory / "other-key.pem").string());
  untrusted.config.timeout = 3;
  EXPECT_THROW(untrusted.request("POST", "/echo", ""), boost::system::system_error);
  EXPECT_EQ(*calls, 1);
}

TEST_F(BeastHttp, OversizedHeadersAreRejectedBeforeCallingHandlers) {
  encrypted = secure_server();
  encrypted->config.header_limit = 80;
  std::atomic_int calls { 0 };
  encrypted->resource["^/echo$"]["POST"] = [&](auto response, auto) { ++calls; response->write("called"); };
  auto client = socket(start(*encrypted));
  const std::string raw = "POST /echo HTTP/1.1\r\nHost: localhost\r\nX-Long: " + std::string(100, 'x') + "\r\nContent-Length: 0\r\n\r\n";
  net::write(*client, net::buffer(raw));
  http::response<http::string_body> response;
  http::read(*client, buffer, response);
  EXPECT_EQ(response.result(), http::status::request_header_fields_too_large);
  EXPECT_EQ(calls, 0);
}

TEST_F(BeastHttp, DeferredOrdinaryResponseIsBoundedByTheContentDeadline) {
  encrypted = secure_server();
  std::promise<std::shared_ptr<web::Server<web::HTTPS>::Response>> held;
  auto future = held.get_future();
  encrypted->resource["^/echo$"]["POST"] = [&](auto response, auto) { held.set_value(response); };
  auto client = socket(start(*encrypted));
  const std::string raw = "POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n";
  net::write(*client, net::buffer(raw));
  ASSERT_EQ(future.wait_for(3s), std::future_status::ready);
  auto response = future.get();
  EXPECT_TRUE(wait_until([&] { return response->closed(); }));
}

TEST_F(BeastHttp, ClientReusesItsConfigurationAndPreservesHttpBodyBytes) {
  plain = std::make_unique<web::Server<web::HTTP>>();
  plain->resource["^/echo$"]["POST"] = [](auto response, auto request) { response->write(request->content.string()); };
  web::Client<web::HTTP> client("127.0.0.1:" + std::to_string(start(*plain)));
  const std::string bytes("data\0tail", 9);
  EXPECT_EQ(client.request("POST", "/echo", bytes)->content.string(), bytes);
  EXPECT_EQ(client.request("POST", "/echo", "again")->content.string(), "again");
}

TEST_F(BeastHttp, ClientVerifiesTlsHostnameAndUsesExplicitTrustAnchors) {
  encrypted = secure_server();
  encrypted->resource["^/echo$"]["POST"] = [](auto response, auto) { response->write("secure"); };
  const auto port = start(*encrypted);
  const auto ca = (directory / "cert.pem").string();
  web::Client<web::HTTPS> trusted("localhost:" + std::to_string(port), true, "", "", ca);
  EXPECT_EQ(trusted.request("POST", "/echo")->content.string(), "secure");
  web::Client<web::HTTPS> wrong_hostname("127.0.0.1:" + std::to_string(port), true, "", "", ca);
  EXPECT_THROW(wrong_hostname.request("POST", "/echo"), boost::system::system_error);
  web::Client<web::HTTPS> explicitly_unverified("127.0.0.1:" + std::to_string(port), false);
  EXPECT_EQ(explicitly_unverified.request("POST", "/echo")->content.string(), "secure");
}

TEST_F(BeastHttp, ClientBoundsResponseBodiesAndDoesNotReplayPostOnTimeout) {
  plain = std::make_unique<web::Server<web::HTTP>>();
  std::atomic_int calls { 0 };
  std::promise<std::shared_ptr<web::Server<web::HTTP>::Response>> held;
  auto future = held.get_future();
  plain->resource["^/large$"]["POST"] = [](auto response, auto) { response->write(std::string(500, 'x')); };
  plain->resource["^/idle$"]["POST"] = [&](auto response, auto) { ++calls; held.set_value(response); };
  const auto authority = "127.0.0.1:" + std::to_string(start(*plain));
  web::Client<web::HTTP> bounded(authority);
  bounded.config.max_response_streambuf_size = 128;
  EXPECT_THROW(bounded.request("POST", "/large"), boost::system::system_error);
  web::Client<web::HTTP> timeout(authority);
  timeout.config.timeout = 1;
  const auto before = std::chrono::steady_clock::now();
  EXPECT_THROW(timeout.request("POST", "/idle"), boost::system::system_error);
  EXPECT_LT(std::chrono::steady_clock::now() - before, 2s);
  EXPECT_EQ(calls, 1);
  ASSERT_EQ(future.wait_for(0s), std::future_status::ready);
  auto response = future.get();
}

TEST_F(BeastHttp, HeaderOnlyClientCompletesWithoutWaitingForTheAdvertisedBody) {
  plain = std::make_unique<web::Server<web::HTTP>>();
  std::promise<std::shared_ptr<web::Server<web::HTTP>::Response>> held;
  auto future = held.get_future();
  plain->resource["^/echo$"]["POST"] = [&](auto response, auto) {
    response->write(web::StatusCode::accepted, { { "Content-Length", "1000000000" }, { "Retry-After", "1" } });
    response->send();
    held.set_value(response);
  };
  web::Client<web::HTTP> client("127.0.0.1:" + std::to_string(start(*plain)));
  client.config.headers_only = true;
  client.config.max_response_streambuf_size = 16 * 1024;
  const auto before = std::chrono::steady_clock::now();
  const auto response = client.request("POST", "/echo");
  EXPECT_EQ(response->status_code, "202 Accepted");
  EXPECT_EQ(response->header.find("Retry-After")->second, "1");
  EXPECT_TRUE(response->content.string().empty());
  EXPECT_LT(std::chrono::steady_clock::now() - before, 1s);
  ASSERT_EQ(future.wait_for(0s), std::future_status::ready);
  auto retained = future.get();
}

TEST_F(BeastHttp, AsyncClientCancellationCompletesExactlyOnce) {
  plain = std::make_unique<web::Server<web::HTTP>>();
  std::promise<std::shared_ptr<web::Server<web::HTTP>::Response>> held;
  auto future = held.get_future();
  plain->resource["^/echo$"]["POST"] = [&](auto response, auto) { held.set_value(response); };
  web::Client<web::HTTP> client("127.0.0.1:" + std::to_string(start(*plain)));
  client.config.timeout = 2;
  client.io_service = std::make_shared<web::io_context>();
  std::atomic_int callbacks { 0 };
  client.request("POST", "/echo", "body", {}, [&](auto, auto ec) { EXPECT_EQ(ec, net::error::operation_aborted); ++callbacks; });
  std::thread runner([io = client.io_service] { io->run(); });
  const auto ready = future.wait_for(3s);
  EXPECT_EQ(ready, std::future_status::ready);
  client.stop();
  EXPECT_TRUE(wait_until([&] { return callbacks == 1; }));
  runner.join();
  EXPECT_EQ(callbacks, 1);
  if (ready == std::future_status::ready) { auto retained = future.get(); }
}

TEST_F(BeastHttp, CopiedRoutesRunOnTwoBoundAddressesAndStoppingOneDoesNotStopTheOther) {
  // Some hosts require an explicit loopback alias for 127.0.0.2. Skip only
  // when that address is unavailable, preserving failures for other bind errors.
  {
    tcp::acceptor probe(io);
    web::error_code ec;
    probe.open(tcp::v4(), ec);
    ASSERT_FALSE(ec) << ec.message();
    probe.bind({ net::ip::make_address("127.0.0.2"), 0 }, ec);
    if (ec == boost::system::errc::make_error_condition(boost::system::errc::address_not_available))
      GTEST_SKIP() << "127.0.0.2 is not configured on this host";
    ASSERT_FALSE(ec) << ec.message();
  }
  encrypted = secure_server();
  encrypted->resource["^/echo$"]["POST"] = [](auto response, auto request) {
    response->write(request->local_endpoint().address().to_string() + ':' + request->content.string());
  };
  const auto port = start(*encrypted);
  struct SecondListener {
    web::Server<web::HTTPS> server;
    std::thread thread;
    SecondListener(const std::filesystem::path &dir): server((dir / "cert.pem").string(), (dir / "key.pem").string()) {}
    ~SecondListener() {
      server.stop();
      if (thread.joinable()) thread.join();
    }
  } second { directory };
  second.server.config.address = "127.0.0.2";
  second.server.config.port = port;
  second.server.resource = encrypted->resource;
  std::promise<unsigned short> ready;
  auto started = ready.get_future();
  second.thread = std::thread([&] {
    bool announced = false;
    try {
      second.server.start([&](auto value) { announced = true; ready.set_value(value); });
    }
    catch (...) {
      if (!announced) ready.set_exception(std::current_exception());
    }
  });
  ASSERT_EQ(started.get(), port);
  web::Client<web::HTTPS> primary("127.0.0.1:" + std::to_string(port), false);
  web::Client<web::HTTPS> additional("127.0.0.2:" + std::to_string(port), false);
  EXPECT_EQ(primary.request("POST", "/echo", "one")->content.string(), "127.0.0.1:one");
  EXPECT_EQ(additional.request("POST", "/echo", "two")->content.string(), "127.0.0.2:two");
  second.server.stop();
  second.thread.join();
  EXPECT_EQ(primary.request("POST", "/echo", "still-running")->content.string(), "127.0.0.1:still-running");
}

class BeastHttpShutdown: public BeastHttp, public testing::WithParamInterface<bool> {};

TEST_P(BeastHttpShutdown, ShutdownCancelsSuspendedHandshakeHeaderAndBodyReadsAndReleasesTheirRequests) {
  encrypted = secure_server();
  std::mutex mutex;
  std::vector<std::weak_ptr<web::Request>> cancelled;
  encrypted->on_error = [&](auto request, const web::error_code &ec) {
    if (ec == net::error::operation_aborted) {
      std::lock_guard lock(mutex);
      cancelled.emplace_back(request);
    }
  };
  const auto port = start(*encrypted);
  tcp::socket handshake(io);
  handshake.connect({ net::ip::make_address("127.0.0.1"), port });  // Never send ClientHello.
  auto headers = socket(port);
  auto body = socket(port);
  const std::string partial_header = "POST /echo HTTP/1.1\r\nHost: localhost\r\n";
  std::string partial_body = "POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 100\r\n";
  if (GetParam()) partial_body += "Expect: 100-continue\r\n";
  partial_body += "\r\none";
  net::write(*headers, net::buffer(partial_header));
  net::write(*body, net::buffer(partial_body));
  if (GetParam()) {
    // Also cover shutdown after the server has acknowledged the body read.
    // The other variant keeps the immediate TLS-completion/shutdown race.
    http::response<http::empty_body> interim;
    http::read(*body, buffer, interim);
    EXPECT_EQ(interim.result(), http::status::continue_);
  }
  const auto before = std::chrono::steady_clock::now();
  encrypted->stop();
  listener.join();
  EXPECT_LT(std::chrono::steady_clock::now() - before, 1s);
  std::lock_guard lock(mutex);
  ASSERT_EQ(cancelled.size(), 3);
  for (const auto &request : cancelled) EXPECT_TRUE(request.expired());
}

INSTANTIATE_TEST_SUITE_P(ShutdownTiming, BeastHttpShutdown, testing::Bool());

TEST_F(BeastHttp, Ipv6WildcardListenerAcceptsIpv4AndIpv6Clients) {
  {
    tcp::acceptor probe(io);
    web::error_code ec;
    probe.open(tcp::v6(), ec);
    if (ec == net::error::address_family_not_supported) GTEST_SKIP() << "IPv6 is unavailable on this host";
    ASSERT_FALSE(ec) << ec.message();
    probe.bind({ net::ip::make_address("::1"), 0 }, ec);
    if (ec == boost::system::errc::make_error_condition(boost::system::errc::address_not_available)) GTEST_SKIP() << "IPv6 loopback is unavailable";
    ASSERT_FALSE(ec) << ec.message();
  }
  plain = std::make_unique<web::Server<web::HTTP>>();
  plain->resource["^/echo$"]["POST"] = [](auto response, auto) { response->write("dual-stack"); };
  const auto port = start(*plain, "::");
  for (const auto &host : { "127.0.0.1", "[::1]" }) {
    web::Client<web::HTTP> client(std::string(host) + ':' + std::to_string(port));
    client.config.timeout = 2;
    EXPECT_EQ(client.request("POST", "/echo")->content.string(), "dual-stack");
  }
}
