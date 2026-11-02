/**
 * @file tests/unit/test_file_mapping_rpc.cpp
 * @brief Test src/file_mapping_rpc.*.
 */
#include <src/file_mapping/file_mapping_rpc.h>

#include <gtest/gtest.h>

TEST(FileMappingRpc, AdvertisesTheHelloWireSchema) {
  file_mapping::rpc::exposed_mapping_t mapping;
  mapping.id = "host-downloads";
  mapping.name = "Downloads";
  mapping.side = "host";
  mapping.mode = "read";
  mapping.capabilities = { "list", "read" };

  auto hello = file_mapping::rpc::make_hello(
    file_mapping::rpc::endpoint_e::client,
    "client-uuid",
    { mapping });

  const auto expected = nlohmann::json::parse(R"({
    "type":"hello", "version":1, "endpoint":"client", "client_uuid":"client-uuid",
    "mappings":[{"id":"host-downloads", "name":"Downloads", "side":"host",
                 "mode":"read", "capabilities":["list","read"]}]
  })");
  EXPECT_EQ(hello, expected);

  // The inbound fixture is independent of make_hello(), so a decoder regression
  // cannot hide behind a matching change to the encoder.
  const auto parsed = file_mapping::rpc::parse_control_message(expected.dump());
  ASSERT_TRUE(parsed.ok) << parsed.error;
  EXPECT_EQ(parsed.type, file_mapping::rpc::message_type_e::hello);
  EXPECT_EQ(parsed.body, expected);
}

TEST(FileMappingRpc, RejectsBadControlMessages) {
  auto invalid_json = file_mapping::rpc::parse_control_message("{");
  EXPECT_FALSE(invalid_json.ok);

  auto missing_type = file_mapping::rpc::parse_control_message(R"({"version":1})");
  EXPECT_FALSE(missing_type.ok);

  auto unknown_type = file_mapping::rpc::parse_control_message(R"({"type":"wat"})");
  EXPECT_FALSE(unknown_type.ok);

  auto wrong_version = file_mapping::rpc::parse_control_message(R"({"type":"hello","version":99})");
  EXPECT_FALSE(wrong_version.ok);

  auto malformed_version = file_mapping::rpc::parse_control_message(R"({"type":"hello","version":"1"})");
  EXPECT_FALSE(malformed_version.ok);
}

TEST(FileMappingRpc, NormalizesRequestIds) {
  EXPECT_EQ(file_mapping::rpc::request_id(nlohmann::json { { "id", 42 } }), 42);
  EXPECT_EQ(file_mapping::rpc::request_id(nlohmann::json { { "id", -1 } }), 0);
  EXPECT_EQ(file_mapping::rpc::request_id(nlohmann::json { { "id", "42" } }), 0);
  EXPECT_EQ(file_mapping::rpc::request_id(nlohmann::json::object()), 0);
}

TEST(FileMappingRpc, ParsesNonnegativeUintmax) {
  const auto parsed = file_mapping::rpc::parse_nonnegative_uintmax(nlohmann::json(42));
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(*parsed, 42);
  EXPECT_FALSE(file_mapping::rpc::parse_nonnegative_uintmax(nlohmann::json(-1)).has_value());
  EXPECT_FALSE(file_mapping::rpc::parse_nonnegative_uintmax(nlohmann::json("42")).has_value());
}

TEST(FileMappingRpc, ParsesReadChunkAlias) {
  auto parsed = file_mapping::rpc::parse_control_message(R"({"type":"read_chunk","id":1})");
  ASSERT_TRUE(parsed.ok) << parsed.error;
  EXPECT_EQ(parsed.type, file_mapping::rpc::message_type_e::read);
  EXPECT_EQ(file_mapping::rpc::to_string(parsed.type), "read");

  auto cancel = file_mapping::rpc::parse_control_message(R"({"type":"cancel_job","id":2,"job_id":"job-1"})");
  ASSERT_TRUE(cancel.ok) << cancel.error;
  EXPECT_EQ(cancel.type, file_mapping::rpc::message_type_e::cancel);
}

TEST(FileMappingRpc, SerializesTransferJob) {
  file_mapping::rpc::transfer_job_t job;
  job.job_id = "job-1";
  job.operation = file_mapping::rpc::operation_e::download;
  job.source = { "host", "host-downloads", "setup.exe" };
  job.destination = { "client", "client-downloads", "setup.exe" };
  job.total_bytes = 1000;
  job.transferred_bytes = 250;
  job.state = file_mapping::rpc::job_state_e::running;

  const auto expected = nlohmann::json::parse(R"({
    "job_id":"job-1", "operation":"download",
    "source":{"side":"host", "mapping":"host-downloads", "path":"setup.exe"},
    "destination":{"side":"client", "mapping":"client-downloads", "path":"setup.exe"},
    "total_bytes":1000, "transferred_bytes":250, "state":"running",
    "error_code":"", "error_message":""
  })");
  EXPECT_EQ(file_mapping::rpc::job_to_json(job), expected);
}

TEST(FileMappingRpc, ParsesPeerTransferJobs) {
  using file_mapping::rpc::operation_e;
  const struct {
    const char *wire_name;
    operation_e operation;
  } operations[] {
    { "download", operation_e::download },
    { "list", operation_e::list },
    { "stat", operation_e::stat },
    { "read", operation_e::read },
  };
  for (const auto &[wire_name, operation] : operations) {
    SCOPED_TRACE(wire_name);
    auto json = nlohmann::json::parse(R"({
      "job_id":"peer-job", "operation":"download",
      "source":{"side":"host", "mapping":"host-downloads", "path":"source/setup.exe"},
      "destination":{"side":"client", "mapping":"client-downloads", "path":"saved/setup.exe"},
      "total_bytes":1000, "transferred_bytes":250, "state":"running",
      "error_code":"", "error_message":""
    })");
    json["operation"] = wire_name;
    file_mapping::rpc::transfer_job_t parsed;
    std::string error;
    ASSERT_TRUE(file_mapping::rpc::job_from_json(json, parsed, error)) << error;
    EXPECT_EQ(parsed.job_id, "peer-job");
    EXPECT_EQ(parsed.operation, operation);
    EXPECT_EQ(parsed.source.side, "host");
    EXPECT_EQ(parsed.source.mapping, "host-downloads");
    EXPECT_EQ(parsed.source.path, "source/setup.exe");
    EXPECT_EQ(parsed.destination.side, "client");
    EXPECT_EQ(parsed.destination.mapping, "client-downloads");
    EXPECT_EQ(parsed.destination.path, "saved/setup.exe");
    EXPECT_EQ(parsed.total_bytes, 1000u);
    EXPECT_EQ(parsed.transferred_bytes, 250u);
    EXPECT_EQ(parsed.state, file_mapping::rpc::job_state_e::running);
  }
}

TEST(FileMappingRpc, PreservesTheLittleEndianBinaryWireLayout) {
  file_mapping::rpc::binary_header_t header;
  header.flags = 3;
  header.request_id = 42;
  header.job_id_hash = 123;
  header.handle_id = 456;
  header.offset = (std::uint64_t { 1 } << 40) + 789;
  header.payload_length = 1024;

  // Literal wire bytes keep a symmetric encoder/decoder error from passing.
  const std::array<std::uint8_t, 44> bytes {
    0x50, 0x4d, 0x46, 0x53, 0x01, 0x00, 0x03, 0x00,
    0x2a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x7b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xc8, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x15, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
    0x00, 0x04, 0x00, 0x00,
  };
  EXPECT_EQ(file_mapping::rpc::encode_binary_header(header), bytes);

  file_mapping::rpc::binary_header_t decoded;
  std::string error;
  ASSERT_TRUE(file_mapping::rpc::decode_binary_header(bytes.data(), bytes.size(), decoded, error)) << error;
  EXPECT_EQ(decoded.magic, 0x53464d50u);
  EXPECT_EQ(decoded.version, 1u);
  EXPECT_EQ(decoded.flags, 3);
  EXPECT_EQ(decoded.request_id, 42);
  EXPECT_EQ(decoded.job_id_hash, 123);
  EXPECT_EQ(decoded.handle_id, 456);
  EXPECT_EQ(decoded.offset, (std::uint64_t { 1 } << 40) + 789);
  EXPECT_EQ(decoded.payload_length, 1024);
}
