/**
 * @file tests/unit/test_file_mapping_ws.cpp
 * @brief Test src/file_mapping_ws.*.
 */
#include <src/file_mapping/file_mapping_ws.h>
#include <src/file_mapping/file_mapping_rpc.h>

#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

namespace {
  namespace fs = std::filesystem;

  struct temp_ws_mapping_t {
    fs::path root;

    temp_ws_mapping_t():
        root(fs::temp_directory_path() / fs::path("sunshine_file_mapping_ws_test")) {
      std::error_code ec;
      fs::remove_all(root, ec);
      fs::create_directories(root);
      std::ofstream(root / "hello.txt", std::ios::binary) << "hello";
    }

    ~temp_ws_mapping_t() {
      std::error_code ec;
      fs::remove_all(root, ec);
    }
  };
}  // namespace

TEST(FileMappingWs, ValidatesTransportConfig) {
  file_mapping_ws::transport_config_t config;
  config.certificate_file = "cert.pem";
  config.private_key_file = "key.pem";

  auto result = file_mapping_ws::validate_config(config);
  EXPECT_TRUE(result.ok) << result.error;

  config.certificate_file.clear();
  result = file_mapping_ws::validate_config(config);
  EXPECT_FALSE(result.ok);

  config.certificate_file = "cert.pem";
  config.max_write_queue_frames = 0;
  result = file_mapping_ws::validate_config(config);
  EXPECT_FALSE(result.ok);
}

TEST(FileMappingWs, BuildsSessionTarget) {
  EXPECT_EQ(
    file_mapping_ws::make_session_target("example.test", 47990),
    "wss://example.test:47990/api/v1/file-mapping/session");

  EXPECT_EQ(
    file_mapping_ws::make_session_target("example.test", 0, "custom"),
    "wss://example.test/custom");
}

TEST(FileMappingWs, ValidatesSessionTargetToken) {
  bool consumed = false;
  auto result = file_mapping_ws::validate_session_target(
    "/api/v1/file-mapping/session?token=abc123",
    "/api/v1/file-mapping/session",
    [&consumed](std::string_view token) {
      consumed = token == "abc123";
      return consumed ? std::optional<std::string> { "client-uuid" } : std::nullopt;
    });

  EXPECT_TRUE(result.ok) << result.error;
  EXPECT_TRUE(consumed);
  EXPECT_EQ(result.client_uuid, "client-uuid");
}

TEST(FileMappingWs, RejectsInvalidSessionTarget) {
  auto accepts_any = [](std::string_view) {
    return std::optional<std::string> { "client-uuid" };
  };

  EXPECT_FALSE(file_mapping_ws::validate_session_target(
                 "/wrong?token=abc123",
                 "/api/v1/file-mapping/session",
                 accepts_any)
                 .ok);
  EXPECT_FALSE(file_mapping_ws::validate_session_target(
                 "/api/v1/file-mapping/session",
                 "/api/v1/file-mapping/session",
                 accepts_any)
                 .ok);
  EXPECT_FALSE(file_mapping_ws::validate_session_target(
                 "/api/v1/file-mapping/session?token=abc123",
                 "/api/v1/file-mapping/session",
                 [](std::string_view) {
                   return std::nullopt;
                 })
                 .ok);
}

TEST(FileMappingWsSessionCore, RequiresHelloFirst) {
  file_mapping_ws::session_core_t session;

  auto result = session.handle_text(R"({"type":"list","id":1})");
  EXPECT_FALSE(result.ok);
  EXPECT_TRUE(result.close);
  EXPECT_EQ(session.state(), file_mapping_ws::session_state_e::awaiting_hello);
}

TEST(FileMappingWsSessionCore, AcceptsHelloAndReplies) {
  file_mapping_ws::session_core_t session { "host", "client-uuid", [](std::string_view uuid) {
                                            return uuid == "client-uuid";
                                          } };
  auto hello = file_mapping::rpc::make_hello(
    file_mapping::rpc::endpoint_e::client,
    "client-uuid",
    {});

  auto result = session.handle_text(hello.dump());
  ASSERT_TRUE(result.ok) << result.error;
  ASSERT_TRUE(result.reply.has_value());
  EXPECT_EQ(session.state(), file_mapping_ws::session_state_e::ready);
  EXPECT_EQ(session.peer_uuid(), "client-uuid");

  auto reply = nlohmann::json::parse(result.reply->text);
  EXPECT_EQ(reply["type"].get<std::string>(), "hello");
  EXPECT_EQ(reply["endpoint"].get<std::string>(), "host");
  EXPECT_TRUE(reply["peer_accepted"].get<bool>());
}

TEST(FileMappingWsSessionCore, RejectsMismatchedHelloUuid) {
  file_mapping_ws::session_core_t session { "host", "expected-client", [](std::string_view) {
                                            return true;
                                          } };
  auto hello = file_mapping::rpc::make_hello(
    file_mapping::rpc::endpoint_e::client,
    "other-client",
    {});

  auto result = session.handle_text(hello.dump());
  EXPECT_FALSE(result.ok);
  EXPECT_TRUE(result.close);
}

TEST(FileMappingWsSessionCore, RejectsUnpairedHelloUuid) {
  file_mapping_ws::session_core_t session { "host", "client-uuid", [](std::string_view) {
                                            return false;
                                          } };
  auto hello = file_mapping::rpc::make_hello(
    file_mapping::rpc::endpoint_e::client,
    "client-uuid",
    {});

  auto result = session.handle_text(hello.dump());
  EXPECT_FALSE(result.ok);
  EXPECT_TRUE(result.close);
}

TEST(FileMappingWsSessionCore, RejectsHostHelloEndpoint) {
  file_mapping_ws::session_core_t session;
  auto hello = file_mapping::rpc::make_hello(file_mapping::rpc::endpoint_e::host, "client-uuid", {});

  auto result = session.handle_text(hello.dump());
  EXPECT_FALSE(result.ok);
  EXPECT_TRUE(result.close);
}

TEST(FileMappingWsSessionCore, HandlesControlAfterHello) {
  temp_ws_mapping_t tree;
  file_mapping::mapping_t mapping;
  mapping.id = "host-downloads";
  mapping.name = "Host Downloads";
  mapping.local_root = tree.root;

  file_mapping::operations::execution_context_t context;
  context.mappings.push_back(std::move(mapping));

  file_mapping_ws::session_core_t session { "host", {}, {}, std::move(context) };
  auto hello = file_mapping::rpc::make_hello(file_mapping::rpc::endpoint_e::client, "client-uuid", {});
  ASSERT_TRUE(session.handle_text(hello.dump()).ok);

  auto result = session.handle_text(R"({"type":"list","id":7,"mapping":"host-downloads","path":""})");
  ASSERT_TRUE(result.ok) << result.error;
  ASSERT_TRUE(result.reply.has_value());

  auto reply = nlohmann::json::parse(result.reply->text);
  EXPECT_EQ(reply["type"].get<std::string>(), "result");
  EXPECT_TRUE(reply["ok"].get<bool>());
  EXPECT_EQ(reply["entries"].size(), 1);
  ASSERT_TRUE(reply.contains("job_id"));
  ASSERT_TRUE(reply.contains("job"));
  EXPECT_EQ(reply["job"]["operation"].get<std::string>(), "list");
  EXPECT_EQ(reply["job"]["state"].get<std::string>(), "completed");

  const auto status_message = std::string { R"({"type":"job_status","id":8,"job_id":")" } +
                              reply["job_id"].get<std::string>() + R"("})";
  auto status = session.handle_text(status_message);
  ASSERT_TRUE(status.ok) << status.error;
  ASSERT_TRUE(status.reply.has_value());
  auto status_reply = nlohmann::json::parse(status.reply->text);
  EXPECT_EQ(status_reply["type"].get<std::string>(), "result");
  EXPECT_EQ(status_reply["job"]["state"].get<std::string>(), "completed");
}

TEST(FileMappingWsSessionCore, CancelsKnownJob) {
  temp_ws_mapping_t tree;
  file_mapping::mapping_t mapping;
  mapping.id = "host-downloads";
  mapping.name = "Host Downloads";
  mapping.local_root = tree.root;

  file_mapping::operations::execution_context_t context;
  context.mappings.push_back(std::move(mapping));

  file_mapping_ws::session_core_t session { "host", {}, {}, std::move(context) };
  auto hello = file_mapping::rpc::make_hello(file_mapping::rpc::endpoint_e::client, "client-uuid", {});
  ASSERT_TRUE(session.handle_text(hello.dump()).ok);

  auto result = session.handle_text(R"({"type":"read","id":9,"mapping":"host-downloads","path":"hello.txt","length":2})");
  ASSERT_TRUE(result.ok) << result.error;
  auto reply = nlohmann::json::parse(result.reply->text);

  const auto cancel_message = std::string { R"({"type":"cancel_job","id":10,"job_id":")" } +
                              reply["job_id"].get<std::string>() + R"("})";
  auto cancelled = session.handle_text(cancel_message);
  ASSERT_TRUE(cancelled.ok) << cancelled.error;
  auto cancel_reply = nlohmann::json::parse(cancelled.reply->text);
  EXPECT_EQ(cancel_reply["type"].get<std::string>(), "result");
  EXPECT_EQ(cancel_reply["job_id"].get<std::string>(), reply["job_id"].get<std::string>());
}

TEST(FileMappingWsSessionCore, HandlesBinaryAfterHello) {
  file_mapping_ws::session_core_t session;
  auto hello = file_mapping::rpc::make_hello(file_mapping::rpc::endpoint_e::client, "client-uuid", {});
  ASSERT_TRUE(session.handle_text(hello.dump()).ok);

  file_mapping::rpc::binary_header_t header;
  header.request_id = 9;
  header.payload_length = 3;
  auto encoded = file_mapping::rpc::encode_binary_header(header);
  std::vector<std::uint8_t> frame(encoded.begin(), encoded.end());
  frame.insert(frame.end(), { 1, 2, 3 });

  auto result = session.handle_binary(frame.data(), frame.size());
  ASSERT_TRUE(result.ok) << result.error;
  ASSERT_TRUE(result.reply.has_value());

  auto reply = nlohmann::json::parse(result.reply->text);
  EXPECT_TRUE(reply["binary"].get<bool>());
  EXPECT_EQ(reply["request_id"].get<int>(), 9);
  EXPECT_EQ(reply["payload_length"].get<int>(), 3);
}

TEST(FileMappingWsSessionCore, RejectsBinaryPayloadLengthMismatch) {
  file_mapping_ws::session_core_t session;
  auto hello = file_mapping::rpc::make_hello(file_mapping::rpc::endpoint_e::client, "client-uuid", {});
  ASSERT_TRUE(session.handle_text(hello.dump()).ok);

  file_mapping::rpc::binary_header_t header;
  header.payload_length = 3;
  auto encoded = file_mapping::rpc::encode_binary_header(header);
  std::vector<std::uint8_t> frame(encoded.begin(), encoded.end());
  frame.insert(frame.end(), { 1, 2, 3, 4 });

  auto result = session.handle_binary(frame.data(), frame.size());
  EXPECT_FALSE(result.ok);
  EXPECT_TRUE(result.close);
}

TEST(FileMappingWsSessionCore, RejectsBinaryBeforeHello) {
  file_mapping_ws::session_core_t session;
  file_mapping::rpc::binary_header_t header;
  auto encoded = file_mapping::rpc::encode_binary_header(header);

  auto result = session.handle_binary(encoded.data(), encoded.size());
  EXPECT_FALSE(result.ok);
  EXPECT_TRUE(result.close);
}
