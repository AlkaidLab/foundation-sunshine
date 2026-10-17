#include <filesystem>
#include <fstream>
#include <future>
#include <thread>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "src/config.h"
#include "src/crypto.h"
#include "src/httpcommon.h"
#include "src/http/server.h"
#include "src/nvhttp.h"
#include "src/nvhttp/pairing.h"
#include "src/uuid.h"

namespace {
  class PairingPolicy: public testing::Test {
  protected:
    std::filesystem::path directory;
    std::string original_state;
    std::string original_id;
    bool original_fresh;
    crypto::creds_t first;
    crypto::creds_t second;

    void SetUp() override {
      directory = std::filesystem::temp_directory_path() / ("sunshine-pairing-policy-" + uuid_util::uuid_t::generate().string());
      std::filesystem::create_directories(directory);
      original_state = config::nvhttp.file_state;
      original_id = http::unique_id;
      original_fresh = config::sunshine.flags[config::flag::FRESH_STATE];
      config::sunshine.flags[config::flag::FRESH_STATE] = false;
      config::nvhttp.file_state = (directory / "state.json").string();
      first = crypto::gen_creds("policy-first", 2048);
      second = crypto::gen_creds("policy-second", 2048);
      // Deliberately omit enabled to exercise old state-file migration.
      const nlohmann::json state {
        { "other_state", "retained" },
        { "root", { { "uniqueid", "policy-host" }, { "named_devices", nlohmann::json::array({
          { { "uuid", "first" }, { "name", "Same display name" }, { "cert", first.x509 } },
          { { "uuid", "second" }, { "name", "Same display name" }, { "cert", second.x509 } },
        }) } } },
      };
      std::ofstream(config::nvhttp.file_state) << state;
      nvhttp::pairing::load_state();
    }

    void TearDown() override {
      config::nvhttp.file_state = (directory / "state.json").string();
      std::ofstream(config::nvhttp.file_state) << R"({"root":{"uniqueid":"empty","named_devices":[]}})";
      nvhttp::pairing::load_state();
      config::nvhttp.file_state = original_state;
      config::sunshine.flags[config::flag::FRESH_STATE] = original_fresh;
      http::unique_id = original_id;
      std::error_code ignored;
      std::filesystem::remove_all(directory, ignored);
    }
  };
}

TEST_F(PairingPolicy, DisabledCertificateIsDeniedInBothVerificationModesAndAfterReload) {
  auto certificate = crypto::x509(first.x509);
  auto other = crypto::x509(second.x509);
  ASSERT_TRUE(nvhttp::is_client_enabled("first"));
  ASSERT_EQ(nvhttp::pairing::verify_client_certificate(certificate.get(), false), nullptr);
  ASSERT_TRUE(nvhttp::set_client_enabled("first", false));
  EXPECT_NE(nvhttp::pairing::verify_client_certificate(certificate.get(), false), nullptr);
  EXPECT_NE(nvhttp::pairing::verify_client_certificate(certificate.get(), true), nullptr);
  EXPECT_TRUE(nvhttp::pairing::client_uuid_for_cert(certificate.get()).empty());
  EXPECT_EQ(nvhttp::pairing::verify_client_certificate(other.get(), false), nullptr);
  EXPECT_EQ(nvhttp::get_all_clients().size(), 2U);
  nvhttp::pairing::load_state();
  EXPECT_FALSE(nvhttp::is_client_enabled("first"));
  EXPECT_TRUE(nvhttp::is_client_enabled("second"));
  EXPECT_NE(nvhttp::pairing::verify_client_certificate(certificate.get(), true), nullptr);
  ASSERT_TRUE(nvhttp::set_client_enabled("first", true));
  EXPECT_EQ(nvhttp::pairing::verify_client_certificate(certificate.get(), false), nullptr);
  EXPECT_EQ(nvhttp::pairing::client_uuid_for_cert(certificate.get()), "first");
  std::ifstream file(config::nvhttp.file_state);
  EXPECT_EQ(nlohmann::json::parse(file).at("other_state"), "retained");
}

TEST_F(PairingPolicy, FailedPersistenceDoesNotReportSuccessOrChangeTrust) {
  auto certificate = crypto::x509(first.x509);
  config::nvhttp.file_state = directory.string();
  EXPECT_FALSE(nvhttp::set_client_enabled("first", false));
  EXPECT_TRUE(nvhttp::is_client_enabled("first"));
  EXPECT_EQ(nvhttp::pairing::verify_client_certificate(certificate.get(), false), nullptr);
  EXPECT_FALSE(nvhttp::set_client_enabled("unknown", false));
}

TEST_F(PairingPolicy, DisablingRevokesAnAlreadyEstablishedTlsConnection) {
  using namespace std::chrono_literals;
  namespace asio = boost::asio;
  namespace beast_http = boost::beast::http;
  const auto certificate_path = directory / "tls-cert.pem";
  const auto key_path = directory / "tls-key.pem";
  std::ofstream(certificate_path) << first.x509;
  std::ofstream(key_path) << first.pkey;
  web::Server<web::PairedHTTPS> server(certificate_path.string(), key_path.string());
  server.config.address = "127.0.0.1";
  server.config.port = 0;
  server.verify = [](SSL *ssl) {
    crypto::x509_t certificate {
#if OPENSSL_VERSION_MAJOR >= 3
      SSL_get1_peer_certificate(ssl)
#else
      SSL_get_peer_certificate(ssl)
#endif
    };
    return certificate && !nvhttp::pairing::verify_client_certificate(certificate.get(), true);
  };
  server.identify = [](X509 *certificate) { return nvhttp::pairing::client_uuid_for_cert(certificate); };
  server.resource["^/probe$"]["GET"] = [](auto response, auto) { response->write("allowed"); };
  server.on_verify_failed = [](auto response, auto) { response->write(web::StatusCode::unauthorized, "denied"); };
  std::promise<unsigned short> ready;
  auto port = ready.get_future();
  std::thread listener([&]() { server.start([&](unsigned short value) { ready.set_value(value); }); });
  auto cleanup = util::fail_guard([&]() { server.stop(); listener.join(); });
  ASSERT_EQ(port.wait_for(10s), std::future_status::ready);
  asio::io_context context;
  asio::ssl::context tls(asio::ssl::context::tls_client);
  tls.set_verify_mode(asio::ssl::verify_none);
  tls.use_certificate_chain_file(certificate_path.string());
  tls.use_private_key_file(key_path.string(), asio::ssl::context::pem);
  asio::ssl::stream<asio::ip::tcp::socket> socket(context, tls);
  socket.lowest_layer().connect({ asio::ip::make_address("127.0.0.1"), port.get() });
  socket.handshake(asio::ssl::stream_base::client);
  boost::beast::flat_buffer buffer;
  const auto request = [&]() {
    beast_http::request<beast_http::empty_body> message { beast_http::verb::get, "/probe", 11 };
    message.set(beast_http::field::host, "localhost");
    beast_http::write(socket, message);
    beast_http::response<beast_http::string_body> response;
    beast_http::read(socket, buffer, response);
    return response.result_int();
  };
  ASSERT_EQ(request(), 200U);
  ASSERT_TRUE(nvhttp::set_client_enabled("first", false));
  EXPECT_EQ(request(), 401U);
}
