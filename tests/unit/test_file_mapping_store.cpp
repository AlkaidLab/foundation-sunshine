/**
 * @file tests/unit/test_file_mapping_store.cpp
 * @brief Test src/file_mapping_store.*.
 */
#include <src/file_mapping/file_mapping_store.h>

#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

namespace {
  namespace fs = std::filesystem;

  struct temp_store_tree_t {
    fs::path root;

    temp_store_tree_t():
        root(fs::temp_directory_path() / fs::path("sunshine_file_mapping_store_test")) {
      std::error_code ec;
      fs::remove_all(root, ec);
      fs::create_directories(root / "Downloads");
      std::ofstream(root / "Downloads" / "hello.txt") << "hello";
    }

    ~temp_store_tree_t() {
      std::error_code ec;
      fs::remove_all(root, ec);
    }
  };
}  // namespace

TEST(FileMappingStore, QuickShareCreatesSafeDefaults) {
  temp_store_tree_t tree;
  file_mapping_store::store_t store;

  auto result = store.add_quick_share(tree.root / "Downloads");
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_FALSE(result.mapping.id.empty());
  EXPECT_EQ(result.mapping.name, "Downloads");
  EXPECT_EQ(result.mapping.mode, file_mapping::access_mode_e::read);
  EXPECT_FALSE(result.mapping.allow_delete);
  EXPECT_FALSE(result.mapping.allow_execute);
  EXPECT_FALSE(result.mapping.follow_reparse_points);
  EXPECT_EQ(result.mapping.max_file_size, 0);
  EXPECT_TRUE(result.mapping.clients.empty());
}

TEST(FileMappingStore, QuickShareIsIdempotentForSamePath) {
  temp_store_tree_t tree;
  file_mapping_store::store_t store;

  auto first = store.add_quick_share(tree.root / "Downloads");
  auto second = store.add_quick_share(tree.root / "Downloads");

  ASSERT_TRUE(first.ok) << first.error;
  ASSERT_TRUE(second.ok) << second.error;
  EXPECT_EQ(first.mapping.id, second.mapping.id);
  EXPECT_EQ(store.snapshot().size(), 1);
}

#ifdef _WIN32
TEST(FileMappingStore, QuickShareAcceptsWindowsVerbatimPath) {
  temp_store_tree_t tree;
  file_mapping_store::store_t store;

  std::error_code ec;
  const auto target = fs::weakly_canonical(tree.root / "Downloads", ec);
  ASSERT_FALSE(ec) << ec.message();

  auto verbatim = target.wstring();
  if (verbatim.rfind(L"\\\\?\\", 0) != 0) {
    verbatim = L"\\\\?\\" + verbatim;
  }

  auto result = store.add_quick_share(fs::path { verbatim });
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(fs::weakly_canonical(result.mapping.local_root), target);
}
#endif

TEST(FileMappingStore, ReplaceNormalizesReadOnlyBoundary) {
  temp_store_tree_t tree;
  file_mapping_store::store_t store;
  file_mapping::mapping_t mapping;
  mapping.id = "legacy-writable";
  mapping.name = "Legacy Writable";
  mapping.local_root = tree.root / "Downloads";
  mapping.mode = file_mapping::access_mode_e::readwrite;
  mapping.allow_delete = true;
  mapping.allow_execute = true;
  mapping.follow_reparse_points = true;

  store.replace({ mapping });

  auto snapshot = store.snapshot();
  ASSERT_EQ(snapshot.size(), 1);
  EXPECT_EQ(snapshot.front().mode, file_mapping::access_mode_e::read);
  EXPECT_FALSE(snapshot.front().allow_delete);
  EXPECT_FALSE(snapshot.front().allow_execute);
  EXPECT_FALSE(snapshot.front().follow_reparse_points);
}

TEST(FileMappingStore, UpdateChangesUserFacingSettings) {
  temp_store_tree_t tree;
  file_mapping_store::store_t store;
  auto created = store.add_quick_share(tree.root / "Downloads");
  ASSERT_TRUE(created.ok) << created.error;

  auto updated = store.update(created.mapping.id, {
                                                 { "name", "Shared Downloads" },
                                                 { "max_file_size", 42 },
                                                 { "clients", { "client-a", "client-b" } },
                                               });

  ASSERT_TRUE(updated.ok) << updated.error;
  EXPECT_EQ(updated.mapping.name, "Shared Downloads");
  EXPECT_EQ(updated.mapping.mode, file_mapping::access_mode_e::read);
  EXPECT_FALSE(updated.mapping.allow_delete);
  EXPECT_EQ(updated.mapping.max_file_size, 42);
  ASSERT_EQ(updated.mapping.clients.size(), 2);
  EXPECT_EQ(updated.mapping.clients[0], "client-a");
}

TEST(FileMappingStore, AcceptsSignedJsonIntegerMaxFileSize) {
  temp_store_tree_t tree;
  file_mapping_store::store_t store;
  auto created = store.add_quick_share(tree.root / "Downloads");
  ASSERT_TRUE(created.ok) << created.error;

  auto updated = store.update(created.mapping.id, {
                                                 { "max_file_size", static_cast<std::int64_t>(42) },
                                               });

  ASSERT_TRUE(updated.ok) << updated.error;
  EXPECT_EQ(updated.mapping.max_file_size, 42);
}

TEST(FileMappingStore, RejectsUnsupportedDeletePermission) {
  temp_store_tree_t tree;
  file_mapping_store::store_t store;
  auto created = store.add_quick_share(tree.root / "Downloads");
  ASSERT_TRUE(created.ok) << created.error;

  auto rejected = store.update(created.mapping.id, {
                                                    { "allow_delete", true },
                                                  });

  EXPECT_FALSE(rejected.ok);
  EXPECT_EQ(rejected.error, "allow_delete is not supported in the read-only phase");
  EXPECT_FALSE(store.snapshot().front().allow_delete);
}

TEST(FileMappingStore, RejectsReadWriteModeInReadOnlyPhase) {
  temp_store_tree_t tree;
  file_mapping_store::store_t store;
  auto created = store.add_quick_share(tree.root / "Downloads");
  ASSERT_TRUE(created.ok) << created.error;

  auto rejected = store.update(created.mapping.id, {
                                                    { "mode", "readwrite" },
                                                  });

  EXPECT_FALSE(rejected.ok);
  EXPECT_EQ(rejected.error, "readwrite mode is not supported in the read-only phase");
  EXPECT_EQ(store.snapshot().front().mode, file_mapping::access_mode_e::read);
}

TEST(FileMappingStore, RejectsUnsupportedDangerousPermissions) {
  temp_store_tree_t tree;
  file_mapping_store::store_t store;
  auto created = store.add_quick_share(tree.root / "Downloads");
  ASSERT_TRUE(created.ok) << created.error;

  auto execute = store.update(created.mapping.id, {
                                                  { "allow_execute", true },
                                                });
  EXPECT_FALSE(execute.ok);
  EXPECT_EQ(execute.error, "allow_execute is not supported");

  auto reparse = store.update(created.mapping.id, {
                                                  { "follow_reparse_points", true },
                                                });
  EXPECT_FALSE(reparse.ok);
  EXPECT_EQ(reparse.error, "follow_reparse_points is not supported");
}

TEST(FileMappingStore, SerializesConfigJson) {
  temp_store_tree_t tree;
  file_mapping_store::store_t store;
  auto created = store.add_quick_share(tree.root / "Downloads");
  ASSERT_TRUE(created.ok) << created.error;

  const auto json = file_mapping_store::serialize_config_json(store.snapshot());
  auto parsed = nlohmann::json::parse(json);
  ASSERT_TRUE(parsed.is_array());
  ASSERT_EQ(parsed.size(), 1);
  EXPECT_EQ(parsed[0]["id"], created.mapping.id);
  EXPECT_EQ(parsed[0]["mode"], "read");
  EXPECT_EQ(parsed[0]["clients"].size(), 0);
}

TEST(FileMappingStore, SerializesConfigValueAsParserSafeBase64) {
  temp_store_tree_t tree;
  file_mapping::mapping_t mapping;
  mapping.id = "host-test";
  mapping.name = "Downloads ] # still data";
  mapping.local_root = tree.root / "Downloads";

  const auto value = file_mapping_store::serialize_config_value({ mapping });
  EXPECT_TRUE(value.rfind("base64:", 0) == 0);
  EXPECT_EQ(value.find('['), std::string::npos);
  EXPECT_EQ(value.find(']'), std::string::npos);
  EXPECT_EQ(value.find('#'), std::string::npos);
}
