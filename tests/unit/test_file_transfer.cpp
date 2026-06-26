#include "src/file_transfer_http.h"
#include "src/file_transfer_store.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "src/config.h"

namespace {
  std::filesystem::path
  make_temp_file(const std::string &contents = "sunshine-file-transfer-test") {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto path = std::filesystem::temp_directory_path() /
                ("sunshine-file-transfer-" + std::to_string(stamp) + ".txt");
    std::ofstream out(path, std::ios::binary);
    out << contents;
    out.close();
    return path;
  }

  std::string
  path_to_utf8(const std::filesystem::path &path) {
    const auto raw = path.u8string();
    return std::string(raw.begin(), raw.end());
  }
}  // namespace

TEST(FileTransferStore, CreatesAndGetsSingleFileOffer) {
  auto path = make_temp_file("hello");
  auto cleanup = [&]() {
    std::error_code ec;
    std::filesystem::remove(path, ec);
  };

  auto created = file_transfer_store::create_single_file_offer(path);
  ASSERT_TRUE(created.ok) << created.err;
  EXPECT_EQ(created.offer.size, 5u);
  EXPECT_FALSE(created.offer.id.empty());
  EXPECT_EQ(created.offer.display_name, path_to_utf8(path.filename()));

  auto got = file_transfer_store::get(created.offer.id);
  ASSERT_TRUE(got.found);
  EXPECT_EQ(got.offer.id, created.offer.id);
  EXPECT_EQ(got.offer.size, 5u);

  cleanup();
}

TEST(FileTransferStore, RejectsDirectory) {
  auto created = file_transfer_store::create_single_file_offer(std::filesystem::temp_directory_path());
  EXPECT_FALSE(created.ok);
  EXPECT_EQ(created.err, "unsupported_file_type");
}

TEST(FileTransferHttp, OfferResponseAndDownloadMetadata) {
  auto path = make_temp_file("payload");
  auto cleanup = [&]() {
    std::error_code ec;
    std::filesystem::remove(path, ec);
  };

  nlohmann::json req;
  req["path"] = path_to_utf8(path);

  auto out = file_transfer_http::make_offer_response(req.dump());
  ASSERT_EQ(out.status, SimpleWeb::StatusCode::success_ok);

  const auto offer = nlohmann::json::parse(out.body);
  ASSERT_TRUE(offer.contains("id"));
  EXPECT_EQ(offer["name"].get<std::string>(), path_to_utf8(path.filename()));
  EXPECT_EQ(offer["size"].get<std::uint64_t>(), 7u);
  EXPECT_EQ(offer["mime"].get<std::string>(), "application/octet-stream");
  EXPECT_EQ(offer["type"].get<std::string>(), "file");

  auto download = file_transfer_http::make_download_response(offer["id"].get<std::string>());
  EXPECT_EQ(download.status, SimpleWeb::StatusCode::success_ok);
  EXPECT_TRUE(download.stream_file);
  EXPECT_EQ(download.path, std::filesystem::canonical(path));
  EXPECT_NE(download.headers.find("Content-Length"), download.headers.end());
  EXPECT_NE(download.headers.find("Content-Disposition"), download.headers.end());

  cleanup();
}

TEST(FileTransferHttp, RejectsWhenDisabled) {
  const bool old = config::input.file_transfer;
  config::input.file_transfer = false;

  auto out = file_transfer_http::make_offer_response(R"({"path":"C:\\nope"})");
  EXPECT_EQ(out.status, SimpleWeb::StatusCode::client_error_forbidden);

  auto download = file_transfer_http::make_download_response(std::string(64, 'a'));
  EXPECT_EQ(download.status, SimpleWeb::StatusCode::client_error_forbidden);
  EXPECT_FALSE(download.stream_file);

  config::input.file_transfer = old;
}
