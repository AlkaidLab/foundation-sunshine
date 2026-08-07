/**
 * @file tests/unit/test_credential_store.cpp
 * @brief Tests for native LLM credential storage.
 */

#include <fstream>
#include <utility>

#include <gtest/gtest.h>

#include "src/ai/credential_store.h"

#if defined(_WIN32)

namespace {
  struct scoped_credential_file_t {
    explicit scoped_credential_file_t(std::string name):
        path(std::filesystem::temp_directory_path() / std::move(name)) {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
    }

    ~scoped_credential_file_t() {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
    }

    std::filesystem::path path;
  };
}  // namespace

TEST(CredentialStoreTest, RoundTripsWithoutWritingPlaintext) {
  const scoped_credential_file_t file_guard("sunshine-credential-store-test.bin");
  const auto &path = file_guard.path;

  const std::string secret = "sk-test-value-that-must-not-appear-on-disk";
  const auto written = credential_store::write_llm_api_key(path, secret);
  ASSERT_TRUE(written.success) << written.error;

  {
    std::ifstream file(path, std::ios::binary);
    ASSERT_TRUE(file.is_open());
    const std::string on_disk(
      (std::istreambuf_iterator<char>(file)),
      std::istreambuf_iterator<char>());
    EXPECT_EQ(on_disk.find(secret), std::string::npos);
  }

  const auto loaded = credential_store::read_llm_api_key(path);
  ASSERT_EQ(loaded.status, credential_store::read_status_e::success) << loaded.error;
  EXPECT_EQ(loaded.secret, secret);

  const auto erased = credential_store::erase_llm_api_key(path);
  ASSERT_TRUE(erased.success) << erased.error;
  EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(CredentialStoreTest, ReportsNotFoundForMissingFile) {
  const scoped_credential_file_t file_guard("sunshine-credential-store-missing.bin");
  const auto loaded = credential_store::read_llm_api_key(file_guard.path);
  EXPECT_EQ(loaded.status, credential_store::read_status_e::not_found);
  EXPECT_TRUE(loaded.secret.empty());
}

TEST(CredentialStoreTest, RejectsInvalidBlobSizes) {
  const scoped_credential_file_t file_guard("sunshine-credential-store-invalid.bin");
  std::ofstream(file_guard.path, std::ios::binary);
  EXPECT_EQ(
    credential_store::read_llm_api_key(file_guard.path).status,
    credential_store::read_status_e::error);

  std::ofstream oversized(file_guard.path, std::ios::binary | std::ios::trunc);
  oversized.seekp(64 * 1024);
  oversized.put('\0');
  oversized.close();
  EXPECT_EQ(
    credential_store::read_llm_api_key(file_guard.path).status,
    credential_store::read_status_e::error);
}

TEST(CredentialStoreTest, RejectsOversizedSecret) {
  const scoped_credential_file_t file_guard("sunshine-credential-store-oversized.bin");
  const auto written = credential_store::write_llm_api_key(file_guard.path, std::string(16 * 1024 + 1, 'a'));
  EXPECT_FALSE(written.success);
  EXPECT_FALSE(std::filesystem::exists(file_guard.path));
}

TEST(CredentialStoreTest, EmptySecretErasesAndEraseIsIdempotent) {
  const scoped_credential_file_t file_guard("sunshine-credential-store-empty.bin");
  ASSERT_TRUE(credential_store::write_llm_api_key(file_guard.path, "sk-value").success);
  EXPECT_TRUE(credential_store::write_llm_api_key(file_guard.path, "").success);
  EXPECT_FALSE(std::filesystem::exists(file_guard.path));
  EXPECT_TRUE(credential_store::erase_llm_api_key(file_guard.path).success);
}

#endif
