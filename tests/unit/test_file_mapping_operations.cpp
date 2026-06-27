/**
 * @file tests/unit/test_file_mapping_operations.cpp
 * @brief Test src/file_mapping_operations.*.
 */
#include <src/file_mapping/file_mapping_operations.h>
#include <src/file_mapping/file_mapping_store.h>

#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

namespace {
  namespace fs = std::filesystem;

  struct temp_tree_t {
    fs::path root;

    temp_tree_t():
        root(fs::temp_directory_path() / fs::path("sunshine_file_mapping_ops_test")) {
      std::error_code ec;
      fs::remove_all(root, ec);
      fs::create_directories(root / "nested");
      std::ofstream(root / "hello.txt", std::ios::binary) << "hello world";
      std::ofstream(root / "nested" / "child.txt", std::ios::binary) << "child";
    }

    ~temp_tree_t() {
      std::error_code ec;
      fs::remove_all(root, ec);
    }
  };

  file_mapping::operations::execution_context_t
  make_context(const fs::path &root) {
    file_mapping::mapping_t mapping;
    mapping.id = "host-test";
    mapping.name = "Host Test";
    mapping.local_root = root;
    mapping.clients = { "client-uuid" };

    file_mapping::operations::execution_context_t context;
    context.peer_uuid = "client-uuid";
    context.mappings.push_back(std::move(mapping));
    return context;
  }
}  // namespace

TEST(FileMappingOperations, ListsDirectoryEntries) {
  temp_tree_t tree;
  auto context = make_context(tree.root);
  auto parsed = file_mapping::rpc::parse_control_message(R"({"type":"list","id":1,"mapping":"host-test","path":""})");
  ASSERT_TRUE(parsed.ok) << parsed.error;

  auto result = file_mapping::operations::execute_control_message(parsed, context);
  EXPECT_EQ(result["type"].get<std::string>(), "result");
  EXPECT_TRUE(result["ok"].get<bool>());
  EXPECT_EQ(result["entries"].size(), 2);
}

TEST(FileMappingOperations, TruncatesLargeDirectoryListings) {
  temp_tree_t tree;
  std::ofstream(tree.root / "extra.txt", std::ios::binary) << "extra";
  auto context = make_context(tree.root);
  context.max_list_entries = 2;

  auto parsed = file_mapping::rpc::parse_control_message(R"({"type":"list","id":9,"mapping":"host-test","path":""})");
  ASSERT_TRUE(parsed.ok) << parsed.error;

  auto result = file_mapping::operations::execute_control_message(parsed, context);
  EXPECT_EQ(result["type"].get<std::string>(), "result");
  EXPECT_EQ(result["entries"].size(), 2);
  EXPECT_TRUE(result["truncated"].get<bool>());
}

TEST(FileMappingOperations, StatsFile) {
  temp_tree_t tree;
  auto context = make_context(tree.root);
  auto parsed = file_mapping::rpc::parse_control_message(R"({"type":"stat","id":2,"mapping":"host-test","path":"hello.txt"})");
  ASSERT_TRUE(parsed.ok) << parsed.error;

  auto result = file_mapping::operations::execute_control_message(parsed, context);
  EXPECT_EQ(result["type"].get<std::string>(), "result");
  EXPECT_EQ(result["kind"].get<std::string>(), "file");
  EXPECT_EQ(result["size"].get<int>(), 11);
}

TEST(FileMappingOperations, ReadsFileChunkAsBase64) {
  temp_tree_t tree;
  auto context = make_context(tree.root);
  auto parsed = file_mapping::rpc::parse_control_message(R"({"type":"read","id":3,"mapping":"host-test","path":"hello.txt","offset":6,"length":5})");
  ASSERT_TRUE(parsed.ok) << parsed.error;

  auto result = file_mapping::operations::execute_control_message(parsed, context);
  EXPECT_EQ(result["type"].get<std::string>(), "result");
  EXPECT_EQ(result["bytes_read"].get<int>(), 5);
  EXPECT_EQ(result["encoding"].get<std::string>(), "base64");
  EXPECT_EQ(result["data"].get<std::string>(), "d29ybGQ=");
  EXPECT_TRUE(result["eof"].get<bool>());
}

TEST(FileMappingOperations, RejectsFilesAboveMappingLimit) {
  temp_tree_t tree;
  auto context = make_context(tree.root);
  context.mappings[0].max_file_size = 4;
  auto parsed = file_mapping::rpc::parse_control_message(R"({"type":"read","id":4,"mapping":"host-test","path":"hello.txt","offset":0,"length":5})");
  ASSERT_TRUE(parsed.ok) << parsed.error;

  auto result = file_mapping::operations::execute_control_message(parsed, context);
  EXPECT_EQ(result["type"].get<std::string>(), "error");
  EXPECT_EQ(result["code"].get<std::string>(), "file_too_large");
}

TEST(FileMappingOperations, RejectsMalformedRequestFields) {
  temp_tree_t tree;
  auto context = make_context(tree.root);
  auto bad_path = file_mapping::rpc::parse_control_message(R"({"type":"list","id":"bad","mapping":"host-test","path":7})");
  ASSERT_TRUE(bad_path.ok) << bad_path.error;

  auto path_result = file_mapping::operations::execute_control_message(bad_path, context);
  EXPECT_EQ(path_result["type"].get<std::string>(), "error");
  EXPECT_EQ(path_result["id"].get<int>(), 0);
  EXPECT_EQ(path_result["code"].get<std::string>(), "bad_request");

  auto bad_length = file_mapping::rpc::parse_control_message(R"({"type":"read","id":11,"mapping":"host-test","path":"hello.txt","length":-1})");
  ASSERT_TRUE(bad_length.ok) << bad_length.error;
  auto length_result = file_mapping::operations::execute_control_message(bad_length, context);
  EXPECT_EQ(length_result["type"].get<std::string>(), "error");
  EXPECT_EQ(length_result["code"].get<std::string>(), "bad_request");
}

TEST(FileMappingOperations, RejectsOpenAsUnsupported) {
  temp_tree_t tree;
  auto context = make_context(tree.root);
  auto parsed = file_mapping::rpc::parse_control_message(R"({"type":"open","id":12,"mapping":"host-test","path":"hello.txt"})");
  ASSERT_TRUE(parsed.ok) << parsed.error;

  auto result = file_mapping::operations::execute_control_message(parsed, context);
  EXPECT_EQ(result["type"].get<std::string>(), "error");
  EXPECT_EQ(result["code"].get<std::string>(), "unsupported_operation");
}

TEST(FileMappingOperations, RejectsUnauthorizedClient) {
  temp_tree_t tree;
  auto context = make_context(tree.root);
  context.peer_uuid = "other-client";
  auto parsed = file_mapping::rpc::parse_control_message(R"({"type":"list","id":5,"mapping":"host-test","path":""})");
  ASSERT_TRUE(parsed.ok) << parsed.error;

  auto result = file_mapping::operations::execute_control_message(parsed, context);
  EXPECT_EQ(result["type"].get<std::string>(), "error");
  EXPECT_EQ(result["code"].get<std::string>(), "forbidden");
}

TEST(FileMappingOperations, RejectsAbsoluteAndParentTraversalPaths) {
  temp_tree_t tree;
  auto context = make_context(tree.root);

  auto absolute = file_mapping::rpc::parse_control_message(R"({"type":"read","id":6,"mapping":"host-test","path":"C:/Windows/win.ini"})");
  ASSERT_TRUE(absolute.ok) << absolute.error;
  auto absolute_result = file_mapping::operations::execute_control_message(absolute, context);
  EXPECT_EQ(absolute_result["type"].get<std::string>(), "error");
  EXPECT_EQ(absolute_result["code"].get<std::string>(), "absolute_path");

  auto traversal = file_mapping::rpc::parse_control_message(R"({"type":"read","id":7,"mapping":"host-test","path":"../outside.txt"})");
  ASSERT_TRUE(traversal.ok) << traversal.error;
  auto traversal_result = file_mapping::operations::execute_control_message(traversal, context);
  EXPECT_EQ(traversal_result["type"].get<std::string>(), "error");
  EXPECT_EQ(traversal_result["code"].get<std::string>(), "invalid_path");
}

TEST(FileMappingOperations, UsesMappingProviderForRuntimeUpdates) {
  temp_tree_t tree;
  file_mapping_store::store_t store;

  file_mapping::operations::execution_context_t context;
  context.peer_uuid = "client-uuid";
  context.mapping_provider = [&store]() {
    return store.snapshot();
  };

  auto created = store.add_quick_share(tree.root);
  ASSERT_TRUE(created.ok) << created.error;

  const auto message = std::string { R"({"type":"list","id":8,"mapping":")" } + created.mapping.id + R"(","path":""})";
  auto parsed = file_mapping::rpc::parse_control_message(message);
  ASSERT_TRUE(parsed.ok) << parsed.error;

  auto result = file_mapping::operations::execute_control_message(parsed, context);
  EXPECT_EQ(result["type"].get<std::string>(), "result");
  EXPECT_TRUE(result["ok"].get<bool>());
  EXPECT_EQ(result["mapping"].get<std::string>(), created.mapping.id);
  EXPECT_EQ(result["entries"].size(), 2);
}
