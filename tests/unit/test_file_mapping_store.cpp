/**
 * @file tests/unit/test_file_mapping_store.cpp
 * @brief Test src/file_mapping_store.*.
 */
#include <src/file_mapping_store.h>

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

TEST(FileMappingStore, UpdateChangesUserFacingSettings) {
  temp_store_tree_t tree;
  file_mapping_store::store_t store;
  auto created = store.add_quick_share(tree.root / "Downloads");
  ASSERT_TRUE(created.ok) << created.error;

  auto updated = store.update(created.mapping.id, {
                                                 { "name", "Shared Downloads" },
                                                 { "mode", "readwrite" },
                                                 { "allow_delete", true },
                                                 { "max_file_size", 42 },
                                                 { "clients", { "client-a", "client-b" } },
                                               });

  ASSERT_TRUE(updated.ok) << updated.error;
  EXPECT_EQ(updated.mapping.name, "Shared Downloads");
  EXPECT_EQ(updated.mapping.mode, file_mapping::access_mode_e::readwrite);
  EXPECT_TRUE(updated.mapping.allow_delete);
  EXPECT_EQ(updated.mapping.max_file_size, 42);
  ASSERT_EQ(updated.mapping.clients.size(), 2);
  EXPECT_EQ(updated.mapping.clients[0], "client-a");
}

TEST(FileMappingStore, DeletePermissionRequiresReadWriteMode) {
  temp_store_tree_t tree;
  file_mapping_store::store_t store;
  auto created = store.add_quick_share(tree.root / "Downloads");
  ASSERT_TRUE(created.ok) << created.error;

  auto rejected = store.update(created.mapping.id, {
                                                    { "allow_delete", true },
                                                  });

  EXPECT_FALSE(rejected.ok);
  EXPECT_EQ(rejected.error, "allow_delete requires readwrite mode");
  EXPECT_FALSE(store.snapshot().front().allow_delete);
}

TEST(FileMappingStore, SwitchingToReadModeDisablesDeletePermission) {
  temp_store_tree_t tree;
  file_mapping_store::store_t store;
  auto created = store.add_quick_share(tree.root / "Downloads");
  ASSERT_TRUE(created.ok) << created.error;

  auto writable = store.update(created.mapping.id, {
                                                   { "mode", "readwrite" },
                                                   { "allow_delete", true },
                                                 });
  ASSERT_TRUE(writable.ok) << writable.error;

  auto readonly = store.update(created.mapping.id, {
                                                   { "mode", "read" },
                                                 });

  ASSERT_TRUE(readonly.ok) << readonly.error;
  EXPECT_EQ(readonly.mapping.mode, file_mapping::access_mode_e::read);
  EXPECT_FALSE(readonly.mapping.allow_delete);
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
