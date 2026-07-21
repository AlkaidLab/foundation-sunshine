/**
 * @file tests/unit/test_file_mapping_config.cpp
 * @brief Test src/file_mapping_config.*.
 */
#include <src/file_mapping/file_mapping_config.h>
#include <src/file_mapping/file_mapping_store.h>

#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

namespace {
  namespace fs = std::filesystem;

  struct temp_config_mapping_t {
    fs::path root;

    temp_config_mapping_t():
        root(fs::temp_directory_path() / fs::path("sunshine_file_mapping_config_test")) {
      std::error_code ec;
      fs::remove_all(root, ec);
      fs::create_directories(root);
      std::ofstream(root / "probe.txt") << "probe";
    }

    ~temp_config_mapping_t() {
      std::error_code ec;
      fs::remove_all(root, ec);
    }
  };
}  // namespace

TEST(FileMappingConfig, ParsesValidMappings) {
  temp_config_mapping_t temp;
  const auto json =
    "[{\"id\":\"host-test\",\"name\":\"Host Test\",\"path\":\"" +
    temp.root.generic_string() +
    "\",\"mode\":\"read\",\"clients\":[\"client-uuid\"]}]";

  auto result = file_mapping_config::parse_mappings_json(json);
  ASSERT_TRUE(result.warnings.empty()) << result.warnings.front();
  ASSERT_EQ(result.mappings.size(), 1);
  EXPECT_EQ(result.mappings[0].id, "host-test");
  EXPECT_EQ(result.mappings[0].name, "Host Test");
  EXPECT_EQ(result.mappings[0].mode, file_mapping::access_mode_e::read);
  ASSERT_EQ(result.mappings[0].clients.size(), 1);
  EXPECT_EQ(result.mappings[0].clients[0], "client-uuid");
}

TEST(FileMappingConfig, RejectsInvalidJson) {
  auto result = file_mapping_config::parse_mappings_json("{");
  EXPECT_TRUE(result.mappings.empty());
  EXPECT_FALSE(result.warnings.empty());
}

TEST(FileMappingConfig, SkipsMissingDirectories) {
  auto result = file_mapping_config::parse_mappings_json(
    R"([{"id":"missing","path":"Z:/this/path/should/not/exist"}])");

  EXPECT_TRUE(result.mappings.empty());
  EXPECT_FALSE(result.warnings.empty());
}

TEST(FileMappingConfig, IgnoresDangerousPermissionsInReadOnlyPhase) {
  temp_config_mapping_t temp;
  const auto json =
    "[{\"id\":\"host-test\",\"path\":\"" +
    temp.root.generic_string() +
    "\",\"mode\":\"readwrite\",\"allow_delete\":true,\"allow_execute\":true,\"follow_reparse_points\":true}]";

  auto result = file_mapping_config::parse_mappings_json(json);
  ASSERT_EQ(result.mappings.size(), 1);
  EXPECT_EQ(result.mappings[0].mode, file_mapping::access_mode_e::read);
  EXPECT_FALSE(result.mappings[0].allow_delete);
  EXPECT_FALSE(result.mappings[0].allow_execute);
  EXPECT_FALSE(result.mappings[0].follow_reparse_points);
  EXPECT_GE(result.warnings.size(), 4);
}

TEST(FileMappingConfig, SkipsMappingWithMalformedOptionalFields) {
  temp_config_mapping_t temp;
  const auto json =
    "[{\"id\":\"host-test\",\"path\":\"" +
    temp.root.generic_string() +
    "\",\"allow_delete\":\"false\"}]";

  auto result = file_mapping_config::parse_mappings_json(json);
  EXPECT_TRUE(result.mappings.empty());
  ASSERT_FALSE(result.warnings.empty());
}

TEST(FileMappingConfig, ParsesBase64PersistedConfigValue) {
  temp_config_mapping_t temp;
  file_mapping::mapping_t mapping;
  mapping.id = "host-test";
  mapping.name = "Downloads ] # still data";
  mapping.local_root = temp.root;

  const auto encoded = file_mapping_store::serialize_config_value({ mapping });
  ASSERT_TRUE(encoded.rfind("base64:", 0) == 0);

  auto result = file_mapping_config::parse_mappings_json(encoded);
  ASSERT_TRUE(result.warnings.empty()) << result.warnings.front();
  ASSERT_EQ(result.mappings.size(), 1);
  EXPECT_EQ(result.mappings[0].name, mapping.name);
  EXPECT_EQ(result.mappings[0].local_root, mapping.local_root);
}
