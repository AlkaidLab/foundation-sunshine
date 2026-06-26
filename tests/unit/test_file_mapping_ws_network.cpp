/**
 * @file tests/unit/test_file_mapping_ws_network.cpp
 * @brief End-to-end loopback smoke test for file mapping WSS transport.
 */
#include <src/file_mapping/file_mapping_token.h>
#include <src/file_mapping/file_mapping_ws_server.h>

#include <filesystem>
#include <fstream>
#include <thread>

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

  constexpr char kSmokeCert[] = R"(-----BEGIN CERTIFICATE-----
MIIC1zCCAb+gAwIBAgIUPr4savoWrPXtJOBkQb2uVK5JaFcwDQYJKoZIhvcNAQEL
BQAwFDESMBAGA1UEAwwJbG9jYWxob3N0MB4XDTI2MDYyNTEzNDIwMloXDTI2MDYy
NjEzNDIwMlowFDESMBAGA1UEAwwJbG9jYWxob3N0MIIBIjANBgkqhkiG9w0BAQEF
AAOCAQ8AMIIBCgKCAQEAvP/OykMlXtcBTiq9Ee2KJaNIrNLo39C0wEEQYMRO+7JY
sI8C2b4ekr+HdCCJ/tcsHdCkUt/82BvHHrdwUknORYn6LA7h2ZDS/cDcCYLbHU4h
vx+/rbVBE7zI9fHS++MxX2qcU4f9xp76tk7S0hpTGAi6vlNT1QE0E54HIkNwhW2X
tiyhwPpjmzUE8iNzP0k3BrFDSmHCMX0bmq1UKTcg8m6jJWPij4BozTi7aT6KXi4/
s0jqiiRIVGm96d+qEMNm9RPVO3rgL4ZzJ3n5LverXtHWD8je9qM/8vVa8rasSxAW
bFTJRqNA2XC/JSBDYHOeHcHwpvZRj9NNi5nKSXDWpQIDAQABoyEwHzAdBgNVHQ4E
FgQU69Gpm4kqLucRmc0bSIhLYllsM/0wDQYJKoZIhvcNAQELBQADggEBAHKTsr4D
A0QChEGgi/y++FAyXEjBWkwJPCgvSr0foRywZ4vDGD2FND0wx4voW7QvaNDDLrrv
skXSnJzrqDTJS6efmgX6ZT/psY/taRMz92ZmeWBxi4gibDXgYopjLYj+J55n2jhJ
RsrTtB5snsx16nuwvJmP2E2l1BF9Gk91PUSeJllqpjnOcj4hcenHlGS6Et606QpB
j2Ln6nPYip3OODepPCr6/TdE8CJcPMWEYUfEwWiIxi1V4dcA8C9u3dgxVUQfRWtn
qhNb4CtvbsyAILkS1yhgi4HevyignddAKYJ5Z5tVzYzGdIHjnpzUVU20v/+OG2U6
jA6Vzm1GD88zAjM=
-----END CERTIFICATE-----
)";

  constexpr char kSmokeKey[] = R"(-----BEGIN PRIVATE KEY-----
MIIEvgIBADANBgkqhkiG9w0BAQEFAASCBKgwggSkAgEAAoIBAQC8/87KQyVe1wFO
Kr0R7Yolo0is0ujf0LTAQRBgxE77sliwjwLZvh6Sv4d0IIn+1ywd0KRS3/zYG8ce
t3BSSc5FifosDuHZkNL9wNwJgtsdTiG/H7+ttUETvMj18dL74zFfapxTh/3Gnvq2
TtLSGlMYCLq+U1PVATQTngciQ3CFbZe2LKHA+mObNQTyI3M/STcGsUNKYcIxfRua
rVQpNyDybqMlY+KPgGjNOLtpPopeLj+zSOqKJEhUab3p36oQw2b1E9U7euAvhnMn
efku96te0dYPyN72oz/y9VrytqxLEBZsVMlGo0DZcL8lIENgc54dwfCm9lGP002L
mcpJcNalAgMBAAECggEAXTKTiT+1JOBG/4GpvDZzYf3zr21NclibWRZ8egszm4Al
peLPmAndT6XsqIIHKkh2s7kX3toe934zIV15oMpOUhIo8CLikgONV54LoxEI9Zl6
oKGKRqFTluUH/+egj59H0HJk5ffwV0o7/Tw/T2W1xetAEuxKMYYnZOkPQYEZ8mDw
PR6/hA3RpFLiPdGUnSjTTOi44yx4WGwjmpsgbEo8/P1cV/L6zoiuyT8vBsZCqSdX
rZ0jwzl0P9L7AGbWcleASjrsl/8ZzwHdUhg0DTe+vnhxCRYXfQ0PgaJsmdcpjeCV
8y7gt5NmD1Wm5dtTMYZGdIvcZcPB9vLxsYDsSCO4HQKBgQDmW2ykAxeXoGX4DsqM
FX3HdLYCC267aTA9pVPw0vcnw6Wq9AuP61k99gT+u7SOpudg7Ax7uhJrs/7UmOLd
3tlL+/jKX1bILAZrDK+/HIOUjMe84UeF6XwCcIrviDMl7+wuy2BMHfOwOp/iS6i5
yWqAhmk0dygtI4GoOOzqP0g8PwKBgQDSCcrEcGEJYuK1BFV8QaKOnec3CzYDxel7
u8UMndz50I5CqyLMiqJF9OJhekjRPMc/cewzXeMc9ovr+ZcudclaTsyMpZiEnfDY
8rf9hkaWk8BB/0Kg6hKJcpbLS8/SZPpJVa2ZlfBHXTxfL67eclLTWBlAny5BncQN
I5Gqmb6EGwKBgQCA0lvdFMWay956bHsk/9fJNSGb3xzbvaV2tABPSwtgt27sPXJB
19Gebvi4I+yDYh8++oK4poQqqww1hBJLFZbbgVvOgKadZtFoCD44WA/VgS0qGanP
35S0II/yCG7iJlwkhyOhLZbb1M0Y1krTKypeGcy3xHM5WwPlOYB0N1OELQKBgQCs
KOaQ+WQwY2Nb6H+BZ/MsXvVkQsY1dYWZrCEp5EN6aJ4Su1+8tG2qVb0xFSCWkPDo
aiKnP++mj9fExkJLDLTMVwaGyj0nhqYhzWFOZz94sQbHkck1SGeFTe2YGT3xQF9+
uMGgwCvA8wVHKDh3kNGe9flM5Kzvj7dg5aTCZ16nvQKBgCw5SSNKBrhd8PtPN7no
S9aGVND/9jXvsrjOro/8X+IZhhI+eorETebhLmsZfuj2+33qlNLV5VWwYk0YT9PF
nYHMvQFm+cBYGfgZsq5mcFE0sSNjtwAqANFMZtneEB+5PQ3ftwZOYwy6j87vkeI8
H/ghgEVOl3Zy/flQFDrlnAmq
-----END PRIVATE KEY-----
)";

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
      std::ofstream(cert, std::ios::binary) << kSmokeCert;
      std::ofstream(key, std::ios::binary) << kSmokeKey;
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
