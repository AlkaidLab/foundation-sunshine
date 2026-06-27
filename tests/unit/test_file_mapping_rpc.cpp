/**
 * @file tests/unit/test_file_mapping_rpc.cpp
 * @brief Test src/file_mapping_rpc.*.
 */
#include <src/file_mapping/file_mapping_rpc.h>

#include <gtest/gtest.h>

TEST(FileMappingRpc, BuildsAndParsesHello) {
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

  auto parsed = file_mapping::rpc::parse_control_message(hello.dump());
  ASSERT_TRUE(parsed.ok) << parsed.error;
  EXPECT_EQ(parsed.type, file_mapping::rpc::message_type_e::hello);
  EXPECT_EQ(parsed.body["version"].get<int>(), file_mapping::rpc::kProtocolVersion);
  EXPECT_EQ(parsed.body["endpoint"].get<std::string>(), "client");
  EXPECT_EQ(parsed.body["mappings"][0]["id"].get<std::string>(), "host-downloads");
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

  auto json = file_mapping::rpc::job_to_json(job);

  file_mapping::rpc::transfer_job_t parsed;
  std::string error;
  ASSERT_TRUE(file_mapping::rpc::job_from_json(json, parsed, error)) << error;
  EXPECT_EQ(parsed.job_id, job.job_id);
  EXPECT_EQ(parsed.operation, job.operation);
  EXPECT_EQ(parsed.source.mapping, job.source.mapping);
  EXPECT_EQ(parsed.destination.path, job.destination.path);
  EXPECT_EQ(parsed.total_bytes, 1000);
  EXPECT_EQ(parsed.transferred_bytes, 250);
  EXPECT_EQ(parsed.state, file_mapping::rpc::job_state_e::running);
}

TEST(FileMappingRpc, ParsesFileOperationJobs) {
  EXPECT_EQ(
    file_mapping::rpc::operation_from_string("list").value(),
    file_mapping::rpc::operation_e::list);
  EXPECT_EQ(
    file_mapping::rpc::operation_from_string("stat").value(),
    file_mapping::rpc::operation_e::stat);
  EXPECT_EQ(
    file_mapping::rpc::operation_from_string("read").value(),
    file_mapping::rpc::operation_e::read);
}

TEST(FileMappingRpc, EncodesAndDecodesBinaryHeader) {
  file_mapping::rpc::binary_header_t header;
  header.flags = 3;
  header.request_id = 42;
  header.job_id_hash = 123;
  header.handle_id = 456;
  header.offset = (std::uint64_t { 1 } << 40) + 789;
  header.payload_length = 1024;

  auto bytes = file_mapping::rpc::encode_binary_header(header);

  file_mapping::rpc::binary_header_t decoded;
  std::string error;
  ASSERT_TRUE(file_mapping::rpc::decode_binary_header(bytes.data(), bytes.size(), decoded, error)) << error;
  EXPECT_EQ(decoded.magic, file_mapping::rpc::kBinaryMagic);
  EXPECT_EQ(decoded.version, file_mapping::rpc::kProtocolVersion);
  EXPECT_EQ(decoded.flags, 3);
  EXPECT_EQ(decoded.request_id, 42);
  EXPECT_EQ(decoded.job_id_hash, 123);
  EXPECT_EQ(decoded.handle_id, 456);
  EXPECT_EQ(decoded.offset, (std::uint64_t { 1 } << 40) + 789);
  EXPECT_EQ(decoded.payload_length, 1024);
}
