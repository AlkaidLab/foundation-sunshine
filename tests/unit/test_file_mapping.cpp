/**
 * @file tests/unit/test_file_mapping.cpp
 * @brief Test src/file_mapping.*.
 */
#include <src/file_mapping/file_mapping.h>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>

namespace {
  namespace fs = std::filesystem;

  class FileMappingTests: public testing::Test {
  protected:
    void
    SetUp() override {
      const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
      root = fs::temp_directory_path() / ("sunshine_file_mapping_test_" + std::to_string(suffix));
      fs::remove_all(root);
      fs::create_directories(root / "folder");
    }

    void
    TearDown() override {
      fs::remove_all(root);
    }

    file_mapping::mapping_t
    mapping() const {
      file_mapping::mapping_t out;
      out.id = "host-downloads";
      out.name = "Downloads";
      out.local_root = root;
      return out;
    }

    fs::path root;
  };
}  // namespace

TEST(FileMappingHelpers, MappingIdValidation) {
  EXPECT_TRUE(file_mapping::is_valid_mapping_id("host-downloads"));
  EXPECT_TRUE(file_mapping::is_valid_mapping_id("client_docs_1"));
  EXPECT_FALSE(file_mapping::is_valid_mapping_id(""));
  EXPECT_FALSE(file_mapping::is_valid_mapping_id("bad id"));
  EXPECT_FALSE(file_mapping::is_valid_mapping_id("bad/id"));
}

TEST(FileMappingHelpers, ReservedWindowsNames) {
  EXPECT_TRUE(file_mapping::is_reserved_windows_name("CON"));
  EXPECT_TRUE(file_mapping::is_reserved_windows_name("con.txt"));
  EXPECT_TRUE(file_mapping::is_reserved_windows_name("LPT1"));
  EXPECT_FALSE(file_mapping::is_reserved_windows_name("console"));
  EXPECT_FALSE(file_mapping::is_reserved_windows_name("file.txt"));
}

TEST(FileMappingHelpers, SafeRelativePathValidation) {
  EXPECT_TRUE(file_mapping::is_safe_relative_path(""));
  EXPECT_TRUE(file_mapping::is_safe_relative_path("folder/file.txt"));
  EXPECT_FALSE(file_mapping::is_safe_relative_path("../file.txt"));
  EXPECT_FALSE(file_mapping::is_safe_relative_path("folder/../file.txt"));
  EXPECT_FALSE(file_mapping::is_safe_relative_path("/absolute"));
  EXPECT_FALSE(file_mapping::is_safe_relative_path("C:/absolute"));
  EXPECT_FALSE(file_mapping::is_safe_relative_path("folder\\file.txt"));
  EXPECT_FALSE(file_mapping::is_safe_relative_path("folder/bad:name.txt"));
  EXPECT_FALSE(file_mapping::is_safe_relative_path("folder/CON.txt"));
  EXPECT_FALSE(file_mapping::is_safe_relative_path("folder/trailing."));
}

TEST_F(FileMappingTests, ResolvesRootAndChildPaths) {
  auto m = mapping();

  auto root_result = file_mapping::resolve_path(m, "");
  ASSERT_TRUE(root_result.ok) << root_result.message;
  EXPECT_EQ(root_result.resolved_path, fs::weakly_canonical(root));

  auto child_result = file_mapping::resolve_path(m, "folder/new-file.txt");
  ASSERT_TRUE(child_result.ok) << child_result.message;
  EXPECT_EQ(child_result.resolved_path, fs::weakly_canonical(root / "folder" / "new-file.txt"));
}

TEST_F(FileMappingTests, RejectsUnsafePaths) {
  auto m = mapping();

  auto parent = file_mapping::resolve_path(m, "../outside.txt");
  EXPECT_FALSE(parent.ok);
  EXPECT_EQ(parent.error, file_mapping::resolve_error_e::invalid_relative_path);

  auto absolute = file_mapping::resolve_path(m, "C:/outside.txt");
  EXPECT_FALSE(absolute.ok);
  EXPECT_EQ(absolute.error, file_mapping::resolve_error_e::absolute_path);

  auto reserved = file_mapping::resolve_path(m, "folder/NUL.txt");
  EXPECT_FALSE(reserved.ok);
  EXPECT_EQ(reserved.error, file_mapping::resolve_error_e::reserved_name);
}

TEST_F(FileMappingTests, MustExistRejectsMissingPath) {
  auto result = file_mapping::resolve_path(mapping(), "folder/missing.txt", true);
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error, file_mapping::resolve_error_e::not_found);
}

TEST_F(FileMappingTests, RejectsReparsePointBeforeCanonicalization) {
  std::error_code ec;
  fs::create_directories(root / "real", ec);
  ASSERT_FALSE(ec) << ec.message();
  fs::create_directory_symlink(root / "real", root / "link", ec);
  if (ec) {
    GTEST_SKIP() << "directory symlink creation is not available: " << ec.message();
  }

  auto result = file_mapping::resolve_path(mapping(), "link", true);
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error, file_mapping::resolve_error_e::reparse_point_blocked);
}
