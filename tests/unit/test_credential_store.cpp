/**
 * @file tests/unit/test_credential_store.cpp
 * @brief Tests for native LLM credential storage.
 */

#include <fstream>

#include <gtest/gtest.h>

#include "src/ai/credential_store.h"

#if defined(_WIN32)

TEST(CredentialStoreTest, RoundTripsWithoutWritingPlaintext) {
  const auto path = std::filesystem::temp_directory_path() / "sunshine-credential-store-test.bin";
  std::error_code ignored;
  std::filesystem::remove(path, ignored);

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

#endif
