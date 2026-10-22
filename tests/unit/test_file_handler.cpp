/**
 * @file tests/unit/test_file_handler.cpp
 * @brief Test src/file_handler.*.
 */
#include <chrono>
#include <fstream>
#include <iterator>

#include <boost/property_tree/ptree.hpp>
#include "src/http/types.h"

#include <src/file_handler.h>

#include <gtest/gtest.h>

struct FileHandlerParentDirectoryTest: testing::TestWithParam<std::tuple<std::string, std::string>> {};

TEST_P(FileHandlerParentDirectoryTest, Run) {
  auto [input, expected] = GetParam();
  EXPECT_EQ(file_handler::get_parent_directory(input), expected);
}

INSTANTIATE_TEST_SUITE_P(
  FileHandlerTests,
  FileHandlerParentDirectoryTest,
  testing::Values(
    std::make_tuple("/path/to/file.txt", "/path/to"),
    std::make_tuple("/path/to/directory/", "/path/to")));

struct FileHandlerTests: testing::Test {
  void
  SetUp() override {
    root = std::filesystem::temp_directory_path() /
           ("sunshine-file-handler-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
  }

  void
  TearDown() override {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }

  std::filesystem::path root;
};

TEST_F(FileHandlerTests, CreatesNestedDirectoriesAndAcceptsExistingDirectories) {
  const auto directory = root / "parent/child";
  const auto path = file_handler::path_to_utf8(directory);
  ASSERT_TRUE(file_handler::make_directory(path));
  EXPECT_TRUE(std::filesystem::is_directory(directory));
  EXPECT_TRUE(file_handler::make_directory(path));
}

TEST_F(FileHandlerTests, ReadsAndWritesTextWithoutChangingContents) {
  const auto file = root / "text.txt";
  const auto path = file_handler::path_to_utf8(file);
  for (const auto content : { "", "content", "first line\nsecond line\n" }) {
    SCOPED_TRACE(content);
    ASSERT_EQ(file_handler::write_file(path.c_str(), content), 0);
    // A native reader provides an oracle independent of the paired helper.
    std::ifstream native(file);
    ASSERT_TRUE(native.is_open());
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(native), std::istreambuf_iterator<char>()), content);
    EXPECT_EQ(file_handler::read_file(path.c_str()), content);
  }
}

TEST_F(FileHandlerTests, ReadMissingFileTest) {
  const auto path = file_handler::path_to_utf8(root / "missing.txt");
  EXPECT_EQ(file_handler::read_file(path.c_str()), "");
}

#ifdef _WIN32
TEST_F(FileHandlerTests, ReadsAndWritesUnicodePath) {
  const auto directory = root / L"文件路径";
  const auto file = directory / L"配置文件.txt";
  const auto utf8_root = file_handler::path_to_utf8(directory);
  const auto utf8_file = file_handler::path_to_utf8(file);

  ASSERT_TRUE(file_handler::make_directory(utf8_root));
  ASSERT_EQ(file_handler::write_file(utf8_file.c_str(), "unicode-path"), 0);
  EXPECT_TRUE(std::filesystem::is_regular_file(file));
  EXPECT_EQ(file_handler::read_file(utf8_file.c_str()), "unicode-path");
}

TEST_F(FileHandlerTests, ReadsAndWritesJsonInUnicodePath) {
  const auto directory = root / L"JSON路径";
  const auto file = directory / L"应用配置.json";
  const auto utf8_root = file_handler::path_to_utf8(directory);
  const auto utf8_file = file_handler::path_to_utf8(file);

  ASSERT_TRUE(file_handler::make_directory(utf8_root));

  boost::property_tree::ptree expected;
  expected.put("name", "Sunshine");
  expected.put("enabled", true);
  ASSERT_NO_THROW(file_handler::write_json(utf8_file, expected));
  EXPECT_TRUE(std::filesystem::is_regular_file(file));

  boost::property_tree::ptree actual;
  ASSERT_NO_THROW(file_handler::read_json(utf8_file, actual));
  EXPECT_EQ(actual.get<std::string>("name"), "Sunshine");
  EXPECT_TRUE(actual.get<bool>("enabled"));
}

TEST_F(FileHandlerTests, ConvertsPercentEncodedUnicodeFilename) {
  const std::filesystem::path expected { L"中文封面.png" };
  const auto encoded = "%E4%B8%AD%E6%96%87%E5%B0%81%E9%9D%A2.png";
  const auto actual = file_handler::path_from_utf8(web::Percent::decode(encoded));

  EXPECT_EQ(actual, expected);
}
#endif
