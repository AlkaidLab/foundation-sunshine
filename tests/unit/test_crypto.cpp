/**
 * @file tests/unit/test_crypto.cpp
 * @brief Tests for cryptography helpers.
 */

#include "src/http/server.h"
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <gtest/gtest.h>

#include "src/config.h"
#include "src/crypto.h"
#include "src/file_handler.h"
#include "src/httpcommon.h"
#include "src/nvhttp.h"
#include "src/nvhttp/apps.h"
#include "src/nvhttp/pairing.h"
#include "src/process.h"
#include "src/utility.h"

namespace {
  namespace fs = std::filesystem;
  namespace pt = boost::property_tree;

  std::string
  without_trailing_line_endings(std::string pem) {
    while (!pem.empty() && (pem.back() == '\n' || pem.back() == '\r')) {
      pem.pop_back();
    }
    return pem;
  }

  std::string
  with_crlf_line_endings(const std::string_view pem) {
    std::string result;
    result.reserve(pem.size() + 32);
    for (const char ch : pem) {
      if (ch == '\n') {
        result.push_back('\r');
      }
      result.push_back(ch);
    }
    return result;
  }

  class PairingStateGuard {
  public:
    PairingStateGuard(
      const std::string &cert,
      const std::string &uuid,
      const fs::path &directory = fs::temp_directory_path(),
      bool remove_directory = false):
        state_file_(directory / ("sunshine_pairing_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json")),
        previous_state_file_(config::nvhttp.file_state),
        previous_unique_id_(http::unique_id),
        remove_directory_(remove_directory) {
      fs::create_directories(state_file_.parent_path());

      pt::ptree device;
      device.put("name", "test-client");
      device.put("cert", cert);
      device.put("uuid", uuid);

      pt::ptree state;
      state.put("root.uniqueid", "test-host");
      pt::ptree devices;
      devices.push_back({ "", device });
      state.add_child("root.named_devices", devices);
      std::ofstream state_stream { state_file_ };
      if (!state_stream.is_open()) {
        throw std::runtime_error("unable to open pairing state fixture");
      }
      pt::write_json(state_stream, state);
      state_stream.flush();
      state_stream.close();
      if (!state_stream) {
        throw std::runtime_error("unable to write pairing state fixture");
      }

      config::nvhttp.file_state = file_handler::path_to_utf8(state_file_);
      nvhttp::pairing::load_state();
    }

    ~PairingStateGuard() {
      nvhttp::erase_all_clients();
      config::nvhttp.file_state = previous_state_file_;
      http::unique_id = previous_unique_id_;

      std::error_code ec;
      fs::remove(state_file_, ec);
      if (remove_directory_) {
        fs::remove(state_file_.parent_path(), ec);
      }
    }

    const fs::path &
    state_file() const noexcept {
      return state_file_;
    }

  private:
    fs::path state_file_;
    std::string previous_state_file_;
    std::string previous_unique_id_;
    bool remove_directory_;
  };
}  // namespace

TEST(CryptoX509, MatchesEquivalentPemFormatting) {
  const auto credentials = crypto::gen_creds("format-test-client", 2048);
  const auto certificate = crypto::x509(credentials.x509);
  ASSERT_TRUE(certificate);
  ASSERT_TRUE(credentials.x509.ends_with('\n'));

  EXPECT_TRUE(crypto::x509_matches_pem(certificate.get(), credentials.x509));

  const auto without_trailing_newline = without_trailing_line_endings(credentials.x509);
  EXPECT_TRUE(crypto::x509_matches_pem(certificate.get(), without_trailing_newline));

  const auto with_crlf = with_crlf_line_endings(credentials.x509);
  EXPECT_TRUE(crypto::x509_matches_pem(certificate.get(), with_crlf));
}

TEST(CryptoX509, RejectsInvalidOrDifferentCertificates) {
  const auto first_credentials = crypto::gen_creds("same-subject", 2048);
  const auto second_credentials = crypto::gen_creds("same-subject", 2048);
  const auto first_certificate = crypto::x509(first_credentials.x509);
  ASSERT_TRUE(first_certificate);

  EXPECT_FALSE(crypto::x509_matches_pem(first_certificate.get(), second_credentials.x509));
  EXPECT_FALSE(crypto::x509_matches_pem(first_certificate.get(), "not a certificate"));
  EXPECT_FALSE(crypto::x509_matches_pem(nullptr, first_credentials.x509));
}

TEST(NvhttpPairing, ResolvesUuidForVerifiedPeerCertificate) {
  const auto credentials = crypto::gen_creds("paired-client", 2048);
  auto certificate = crypto::x509(credentials.x509);
  ASSERT_TRUE(certificate);

  const std::string expected_uuid = "paired-client-uuid";
  const auto stored_cert = without_trailing_line_endings(with_crlf_line_endings(credentials.x509));
  PairingStateGuard state { stored_cert, expected_uuid };

  // This mirrors the X509 pointer passed from the TLS handshake callback.
  EXPECT_EQ(nvhttp::pairing::verify_client_certificate(certificate.get(), true), nullptr);
  EXPECT_EQ(nvhttp::pairing::client_uuid_for_cert(certificate.get()), expected_uuid);
}

TEST(NvhttpPairing, ReadsAndWritesStateFromUnicodePath) {
#ifdef _WIN32
  const auto credentials = crypto::gen_creds("unicode-path-client", 2048);
  auto certificate = crypto::x509(credentials.x509);
  ASSERT_TRUE(certificate);

  const auto directory = fs::temp_directory_path() /
                         (fs::path { L"sunshine_配对_路径_" } /
                           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  const std::string expected_uuid = "unicode-path-client-uuid";
  PairingStateGuard state { credentials.x509, expected_uuid, directory, true };

  EXPECT_EQ(nvhttp::pairing::client_uuid_for_cert(certificate.get()), expected_uuid);

  nvhttp::erase_all_clients();
  pt::ptree saved_state;
  std::ifstream state_stream { state.state_file() };
  ASSERT_TRUE(state_stream.is_open());
  ASSERT_NO_THROW(pt::read_json(state_stream, saved_state));
  EXPECT_EQ(saved_state.get<std::string>("root.uniqueid"), "test-host");
  EXPECT_TRUE(saved_state.get_child("root.named_devices").empty());
#else
  GTEST_SKIP() << "Unicode native-path behavior is Windows-specific";
#endif
}

TEST(NvhttpPairing, BeastTlsUsesStoredPairedCertificateAndCarriesUuidAcrossRequests) {
  namespace net = boost::asio;
  namespace wire = boost::beast::http;
  using tcp = net::ip::tcp;
  const auto directory = fs::temp_directory_path() / ("sunshine-paired-wire-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  struct DirectoryGuard {
    fs::path path;
    ~DirectoryGuard() {
      std::error_code ec;
      fs::remove_all(path, ec);
    }
  } cleanup { directory };
  fs::create_directories(directory);
  const auto host = crypto::gen_creds("localhost", 2048);
  const auto paired = crypto::gen_creds("paired-wire-client", 2048);
  const auto other = crypto::gen_creds("unpaired-wire-client", 2048);
  std::ofstream(directory / "host.pem") << host.x509;
  std::ofstream(directory / "host-key.pem") << host.pkey;
  std::ofstream(directory / "client.pem") << paired.x509;
  std::ofstream(directory / "client-key.pem") << paired.pkey;
  std::ofstream(directory / "other.pem") << other.x509;
  std::ofstream(directory / "other-key.pem") << other.pkey;
  PairingStateGuard state { paired.x509, "stored-paired-uuid", directory };
  struct Listener {
    web::Server<web::PairedHTTPS> server;
    std::thread thread;
    Listener(const fs::path &dir): server((dir / "host.pem").string(), (dir / "host-key.pem").string()) {}
    ~Listener() {
      server.stop();
      if (thread.joinable()) thread.join();
    }
  } listener { directory };
  listener.server.config.address = "127.0.0.1";
  listener.server.verify = [](SSL *ssl) {
    crypto::x509_t cert {
#if OPENSSL_VERSION_MAJOR >= 3
      SSL_get1_peer_certificate(ssl)
#else
      SSL_get_peer_certificate(ssl)
#endif
    };
    return nvhttp::pairing::verify_client_certificate(cert.get(), true) == nullptr;
  };
  listener.server.identify = nvhttp::pairing::client_uuid_for_cert;
  listener.server.on_verify_failed = [](auto response, auto) {
    response->write(web::StatusCode::forbidden, "<root status_code=\"401\"/>", { { "Content-Type", "application/xml" } });
  };
  listener.server.resource["^/identity$"]["GET"] = [](auto response, auto request) { response->write(request->client_uuid); };
  std::promise<unsigned short> ready;
  auto port = ready.get_future();
  listener.thread = std::thread([&] {
    bool announced = false;
    try {
      listener.server.start([&](auto value) { announced = true; ready.set_value(value); });
    }
    catch (...) {
      if (!announced) ready.set_exception(std::current_exception());
    }
  });
  const auto bound_port = port.get();
  net::io_context io;
  for (const bool trusted : { true, false }) {
    net::ssl::context tls { net::ssl::context::tls_client };
    tls.load_verify_file((directory / "host.pem").string());
    tls.set_verify_mode(net::ssl::verify_peer);
    tls.set_verify_callback(net::ssl::host_name_verification("localhost"));
    tls.use_certificate_chain_file((directory / (trusted ? "client.pem" : "other.pem")).string());
    tls.use_private_key_file((directory / (trusted ? "client-key.pem" : "other-key.pem")).string(), net::ssl::context::pem);
    net::ssl::stream<tcp::socket> client(io, tls);
    client.lowest_layer().connect({ net::ip::make_address("127.0.0.1"), bound_port });
    client.handshake(net::ssl::stream_base::client);
    boost::beast::flat_buffer buffer;
    for (int i = 0; i < (trusted ? 3 : 1); ++i) {
      if (trusted) {
        if (i == 2) {
          ASSERT_EQ(nvhttp::unpair_client("stored-paired-uuid"), 1);
        }
        wire::request<wire::empty_body> request { wire::verb::get, "/identity", 11 };
        request.set(wire::field::host, "localhost");
        request.set("X-Client-UUID", "forged-uuid");
        request.keep_alive(i < 2);
        wire::write(client, request);
      }
      wire::response<wire::string_body> response;
      wire::read(client, buffer, response);
      const bool allowed = trusted && i < 2;
      EXPECT_EQ(response.result_int(), allowed ? 200 : 403);
      EXPECT_EQ(response.body(), allowed ? "stored-paired-uuid" : "<root status_code=\"401\"/>");
      EXPECT_EQ(response.keep_alive(), allowed);
    }
  }
}

TEST(NvhttpApps, StreamsLargeAssetOverTlsAndPreservesNotFoundAndBadArguments) {
  namespace net = boost::asio;
  namespace wire = boost::beast::http;
  using tcp = net::ip::tcp;
  const auto directory = fs::temp_directory_path() / ("sunshine-asset-wire-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(directory);
  auto cleanup = util::fail_guard([&] { std::error_code ec; fs::remove_all(directory, ec); });
  const auto previous_cwd = fs::current_path();
  fs::current_path(directory);  // Make the missing default image deterministic.
  auto restore_cwd = util::fail_guard([&] { fs::current_path(previous_cwd); });
  auto old_apps = proc::proc.get_apps();
  auto restore_apps = util::fail_guard([&] { proc::proc.get_apps() = std::move(old_apps); });
  const auto image = directory / fs::path(u8"cover-图片.png");
  constexpr std::uint64_t length = 65 * 1024 * 1024;
  const std::string prefix("\x89PNG\r\n\x1a\n", 8);
  {
    std::ofstream file(image, std::ios::binary);
    file.write(prefix.data(), prefix.size());
  }
  fs::resize_file(image, length);
  {
    std::ofstream file(image, std::ios::binary | std::ios::app);
    file << "tail";
  }
  proc::ctx_t app {};
  app.id = "987654";
  app.image_path = file_handler::path_to_utf8(image);
  proc::proc.get_apps() = { app };
  const auto credentials = crypto::gen_creds("localhost", 2048);
  std::ofstream(directory / "cert.pem") << credentials.x509;
  std::ofstream(directory / "key.pem") << credentials.pkey;
  struct Listener {
    web::Server<web::PairedHTTPS> server;
    std::thread thread;
    Listener(const fs::path &dir): server((dir / "cert.pem").string(), (dir / "key.pem").string()) {}
    ~Listener() {
      server.stop();
      if (thread.joinable()) thread.join();
    }
  } listener { directory };
  listener.server.config.address = "127.0.0.1";
  listener.server.config.max_response_buffer_size = 64 * 1024;
  listener.server.verify = [](SSL *) { return 1; };
  listener.server.identify = [](X509 *) { return "asset-test-client"; };
  listener.server.resource["^/appasset$"]["GET"] = nvhttp::apps::asset;
  std::promise<unsigned short> ready;
  auto port = ready.get_future();
  listener.thread = std::thread([&] {
    bool announced = false;
    try {
      listener.server.start([&](auto value) { announced = true; ready.set_value(value); });
    }
    catch (...) {
      if (!announced) ready.set_exception(std::current_exception());
    }
  });
  const auto bound_port = port.get();
  net::io_context io;
  net::ssl::context tls { net::ssl::context::tls_client };
  tls.set_verify_mode(net::ssl::verify_none);
  tls.use_certificate_chain_file((directory / "cert.pem").string());
  tls.use_private_key_file((directory / "key.pem").string(), net::ssl::context::pem);
  auto connect = [&] {
    auto socket = std::make_unique<net::ssl::stream<tcp::socket>>(io, tls);
    socket->lowest_layer().connect({ net::ip::make_address("127.0.0.1"), bound_port });
    socket->handshake(net::ssl::stream_base::client);
    return socket;
  };
  auto request = [](auto &socket, const std::string &target) {
    wire::request<wire::empty_body> message { wire::verb::get, target, 11 };
    message.set(wire::field::host, "localhost");
    message.keep_alive(false);
    wire::write(socket, message);
  };
  auto socket = connect();
  request(*socket, "/appasset?appid=987654");
  boost::beast::flat_buffer buffer;
  wire::response_parser<wire::file_body> response;
  response.body_limit(length + 4);
  web::error_code ec;
  const auto output = directory / "received.png";
  response.get().body().open(file_handler::path_to_utf8(output).c_str(), boost::beast::file_mode::write, ec);
  ASSERT_FALSE(ec) << ec.message();
  wire::read(*socket, buffer, response, ec);
  ASSERT_FALSE(ec) << ec.message();
  EXPECT_EQ(response.get().result_int(), 200);
  EXPECT_EQ(response.get()[wire::field::content_type], "image/png");
  response.get().body().close();
  ASSERT_EQ(fs::file_size(output), length + 4);
  std::ifstream file(output, std::ios::binary);
  std::string bytes(8, '\0');
  file.read(bytes.data(), bytes.size());
  EXPECT_EQ(bytes, prefix);
  file.seekg(length);
  bytes.resize(4);
  file.read(bytes.data(), bytes.size());
  EXPECT_EQ(bytes, "tail");
  for (const auto &[target, expected] : { std::pair { "/appasset?appid=987655", 404 }, std::pair { "/appasset", 400 } }) {
    auto client = connect();
    request(*client, target);
    boost::beast::flat_buffer data;
    wire::response<wire::string_body> result;
    wire::read(*client, data, result);
    EXPECT_EQ(result.result_int(), expected);
    EXPECT_EQ(result.body(), expected == 404 ? "App asset not found" : "Missing or invalid parameters");
  }
}
