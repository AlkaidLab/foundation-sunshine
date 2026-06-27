/**
 * @file tests/unit/test_file_mapping_ws_network.cpp
 * @brief End-to-end loopback smoke test for file mapping WSS transport.
 */
#include <src/file_mapping/file_mapping_token.h>
#include <src/file_mapping/file_mapping_ws_server.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <thread>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace {
  namespace asio = boost::asio;
  namespace beast = boost::beast;
  namespace ssl = boost::asio::ssl;
  namespace websocket = boost::beast::websocket;
  using tcp = boost::asio::ip::tcp;

  void
  throw_if_false(bool ok, const char *message) {
    if (!ok) {
      throw std::runtime_error(message);
    }
  }

  void
  write_self_signed_certificate(const std::filesystem::path &cert, const std::filesystem::path &key) {
    using evp_pkey_ctx_ptr = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
    using evp_pkey_ptr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
    using x509_ptr = std::unique_ptr<X509, decltype(&X509_free)>;
    using bio_ptr = std::unique_ptr<BIO, decltype(&BIO_free)>;

    evp_pkey_ctx_ptr ctx { EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free };
    throw_if_false(static_cast<bool>(ctx), "failed to create keygen context");
    throw_if_false(EVP_PKEY_keygen_init(ctx.get()) > 0, "failed to initialize keygen");
    throw_if_false(EVP_PKEY_CTX_set_rsa_keygen_bits(ctx.get(), 2048) > 0, "failed to set rsa key size");

    EVP_PKEY *raw_key = nullptr;
    throw_if_false(EVP_PKEY_keygen(ctx.get(), &raw_key) > 0, "failed to generate private key");
    evp_pkey_ptr pkey { raw_key, EVP_PKEY_free };

    x509_ptr x509 { X509_new(), X509_free };
    throw_if_false(static_cast<bool>(x509), "failed to create x509 certificate");
    throw_if_false(X509_set_version(x509.get(), 2) == 1, "failed to set x509 version");
    throw_if_false(ASN1_INTEGER_set(X509_get_serialNumber(x509.get()), 1) == 1, "failed to set serial number");
    throw_if_false(X509_gmtime_adj(X509_get_notBefore(x509.get()), 0) != nullptr, "failed to set notBefore");
    throw_if_false(X509_gmtime_adj(X509_get_notAfter(x509.get()), 3600) != nullptr, "failed to set notAfter");
    throw_if_false(X509_set_pubkey(x509.get(), pkey.get()) == 1, "failed to set public key");

    auto *name = X509_get_subject_name(x509.get());
    throw_if_false(name != nullptr, "failed to get certificate subject");
    throw_if_false(
      X509_NAME_add_entry_by_txt(
        name,
        "CN",
        MBSTRING_ASC,
        reinterpret_cast<const unsigned char *>("localhost"),
        -1,
        -1,
        0) == 1,
      "failed to set certificate subject");
    throw_if_false(X509_set_issuer_name(x509.get(), name) == 1, "failed to set certificate issuer");
    throw_if_false(X509_sign(x509.get(), pkey.get(), EVP_sha256()) > 0, "failed to sign certificate");

    const auto cert_file = cert.string();
    bio_ptr cert_bio { BIO_new_file(cert_file.c_str(), "w"), BIO_free };
    throw_if_false(static_cast<bool>(cert_bio), "failed to open certificate file");
    throw_if_false(PEM_write_bio_X509(cert_bio.get(), x509.get()) == 1, "failed to write certificate file");

    const auto key_file = key.string();
    bio_ptr key_bio { BIO_new_file(key_file.c_str(), "w"), BIO_free };
    throw_if_false(static_cast<bool>(key_bio), "failed to open private key file");
    throw_if_false(PEM_write_bio_PrivateKey(key_bio.get(), pkey.get(), nullptr, nullptr, 0, nullptr, nullptr) == 1, "failed to write private key file");
  }

  struct temp_ws_smoke_t {
    std::filesystem::path root;
    std::filesystem::path cert;
    std::filesystem::path key;

    temp_ws_smoke_t():
        root(std::filesystem::temp_directory_path() / std::filesystem::path("sunshine_file_mapping_ws_network_test")),
        cert(root / "cert.pem"),
        key(root / "key.pem") {
      std::error_code ec;
      std::filesystem::remove_all(root, ec);
      std::filesystem::create_directories(root);
      std::ofstream(root / "hello.txt", std::ios::binary) << "hello world";
      write_self_signed_certificate(cert, key);
    }

    ~temp_ws_smoke_t() {
      std::error_code ec;
      std::filesystem::remove_all(root, ec);
    }
  };

  nlohmann::json
  read_json(websocket::stream<beast::ssl_stream<tcp::socket>> &ws) {
    beast::flat_buffer buffer;
    ws.read(buffer);
    return nlohmann::json::parse(beast::buffers_to_string(buffer.data()));
  }
}  // namespace

TEST(FileMappingWsNetwork, LoopbackHelloListRead) {
  temp_ws_smoke_t temp;

  file_mapping::mapping_t mapping;
  mapping.id = "host-test";
  mapping.name = "Host Test";
  mapping.local_root = temp.root;
  mapping.clients = { "client-uuid" };

  file_mapping::operations::execution_context_t operations_context;
  operations_context.mappings.push_back(mapping);

  file_mapping_token::token_store_t tokens;
  auto token = tokens.issue("client-uuid");

  asio::io_context server_io;
  file_mapping_ws::transport_config_t config;
  config.bind_address = "127.0.0.1";
  config.port = 0;
  config.certificate_file = temp.cert.string();
  config.private_key_file = temp.key.string();
  config.require_client_certificate = false;

  auto server = std::make_shared<file_mapping_ws::server_t>(
    server_io,
    config,
    [&tokens](std::string_view value) {
      return tokens.consume(std::string { value });
    },
    [](std::string_view uuid) {
      return uuid == "client-uuid";
    },
    operations_context);

  auto start_result = server->start();
  ASSERT_TRUE(start_result.ok) << start_result.error;
  ASSERT_GT(server->bound_port(), 0);

  std::thread server_thread([&server_io]() {
    server_io.run();
  });

  auto client_result = [&]() -> testing::AssertionResult {
    try {
      asio::io_context client_io;
      ssl::context client_ssl(ssl::context::tls_client);
      client_ssl.set_verify_mode(ssl::verify_none);
      tcp::resolver resolver(client_io);
      websocket::stream<beast::ssl_stream<tcp::socket>> ws(client_io, client_ssl);

      auto endpoints = resolver.resolve("127.0.0.1", std::to_string(server->bound_port()));
      asio::connect(ws.next_layer().next_layer(), endpoints);
      ws.next_layer().handshake(ssl::stream_base::client);
      ws.handshake("127.0.0.1", "/api/v1/file-mapping/session?token=" + token);

      ws.write(asio::buffer(file_mapping::rpc::make_hello(file_mapping::rpc::endpoint_e::client, "client-uuid", {}).dump()));
      auto hello = read_json(ws);
      if (hello["type"].get<std::string>() != "hello" || hello["mappings"].size() != 1) {
        return testing::AssertionFailure() << "unexpected hello reply: " << hello.dump();
      }

      ws.write(asio::buffer(R"({"type":"list","id":1,"mapping":"host-test","path":""})"));
      auto list = read_json(ws);
      if (list["type"].get<std::string>() != "result" || list["entries"].empty()) {
        return testing::AssertionFailure() << "unexpected list reply: " << list.dump();
      }

      ws.write(asio::buffer(R"({"type":"read","id":2,"mapping":"host-test","path":"hello.txt","offset":6,"length":5})"));
      auto read = read_json(ws);
      if (read["type"].get<std::string>() != "result" || read["data"].get<std::string>() != "d29ybGQ=") {
        return testing::AssertionFailure() << "unexpected read reply: " << read.dump();
      }

      beast::error_code ignored;
      ws.close(websocket::close_code::normal, ignored);
      return testing::AssertionSuccess();
    }
    catch (const std::exception &e) {
      return testing::AssertionFailure() << e.what();
    }
  }();

  server->stop();
  server_io.stop();
  if (server_thread.joinable()) {
    server_thread.join();
  }

  EXPECT_TRUE(client_result);
}
