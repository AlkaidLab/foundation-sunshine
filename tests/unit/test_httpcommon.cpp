/**
 * @file tests/unit/test_httpcommon.cpp
 * @brief Test src/httpcommon.*.
 */
// test imports
#include "../tests_common.h"

#include <chrono>
#include <filesystem>

// lib imports
#include <curl/curl.h>

// local imports
#include <src/file_handler.h>
#include <src/httpcommon.h>

#include "../tests_common.h"

struct UrlEscapeTest: testing::TestWithParam<std::tuple<std::string, std::string>> {};

TEST_P(UrlEscapeTest, Run) {
  const auto &[input, expected] = GetParam();
  ASSERT_EQ(http::url_escape(input), expected);
}

INSTANTIATE_TEST_SUITE_P(
  UrlEscapeTests,
  UrlEscapeTest,
  testing::Values(
    std::make_tuple("igdb_0123456789", "igdb_0123456789"),
    std::make_tuple("../../../", "..%2F..%2F..%2F"),
    std::make_tuple("..*\\", "..%2A%5C")));

struct UrlGetHostTest: testing::TestWithParam<std::tuple<std::string, std::string>> {};

TEST_P(UrlGetHostTest, Run) {
  const auto &[input, expected] = GetParam();
  ASSERT_EQ(http::url_get_host(input), expected);
}

INSTANTIATE_TEST_SUITE_P(
  UrlGetHostTests,
  UrlGetHostTest,
  testing::Values(
    std::make_tuple("https://images.igdb.com/example.txt", "images.igdb.com"),
    std::make_tuple("http://localhost:8080", "localhost"),
    std::make_tuple("nonsense!!}{::", "")));

struct DownloadFileTest: testing::TestWithParam<std::tuple<std::string, std::string>> {};

TEST_P(DownloadFileTest, Run) {
  const auto &[url, filename] = GetParam();
  const std::string test_dir = platf::appdata().string() + "/tests/";
  std::string path = test_dir + filename;
  ASSERT_TRUE(http::download_file(url, path, CURL_SSLVERSION_TLSv1_0));
}

INSTANTIATE_TEST_SUITE_P(
  DownloadFileTests,
  DownloadFileTest,
  testing::Values(
    std::make_tuple("https://httpbin.org/base64/aGVsbG8h", "hello.txt"),
    std::make_tuple("https://httpbin.org/redirect-to?url=/base64/aGVsbG8h", "hello-redirect.txt")));

TEST(ImageDownloadTest, FailedDownloadPreservesExistingFile) {
  const auto root = std::filesystem::temp_directory_path() /
                    ("sunshine-cover-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  const auto destination = root / "cover.png";
  const auto utf8_root = file_handler::path_to_utf8(root);
  const auto utf8_destination = file_handler::path_to_utf8(destination);

  ASSERT_TRUE(file_handler::make_directory(utf8_root));
  ASSERT_EQ(file_handler::write_file(utf8_destination.c_str(), "existing-cover"), 0);
  EXPECT_FALSE(http::download_image_with_magic_check("://invalid", utf8_destination));
  EXPECT_EQ(file_handler::read_file(utf8_destination.c_str()), "existing-cover");

  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
}
