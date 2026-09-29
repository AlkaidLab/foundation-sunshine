/**
 * @file tests/unit/test_codex_auth.cpp
 * @brief Offline checks for Codex account parsing and protected persistence.
 */

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "src/ai/codex_auth.h"

#if defined(_WIN32)
namespace {
  constexpr auto test_access_token =
    "e30.eyJodHRwczovL2FwaS5vcGVuYWkuY29tL2F1dGgiOnsiY2hhdGdwdF9hY2NvdW50X2lkIjoiYWNjdF90ZXN0In19.sig";

  struct credential_files_t {
    credential_files_t():
        codex(std::filesystem::temp_directory_path() / "sunshine-codex-auth-test.bin"),
        api_key(std::filesystem::temp_directory_path() / "sunshine-codex-auth-api-key-test.bin") {
      std::error_code ignored;
      std::filesystem::remove(codex, ignored);
      std::filesystem::remove(api_key, ignored);
    }

    ~credential_files_t() {
      std::error_code ignored;
      std::filesystem::remove(codex, ignored);
      std::filesystem::remove(api_key, ignored);
    }

    std::filesystem::path codex;
    std::filesystem::path api_key;
  };
}  // namespace

TEST(CodexAuthTest, ExtractsOnlyWellFormedAccountClaim) {
  EXPECT_EQ(codex_auth::account_id_from_jwt(test_access_token), "acct_test");
  EXPECT_TRUE(codex_auth::account_id_from_jwt("malformed").empty());
  EXPECT_TRUE(codex_auth::account_id_from_jwt("e30.!!.sig").empty());
  EXPECT_TRUE(codex_auth::account_id_from_jwt("e30.e30.sig").empty());
  EXPECT_TRUE(codex_auth::account_id_from_jwt("e30.e30.sig.extra").empty());
}

TEST(CodexAuthTest, HandlesDevicePollErrorsBeforeFallbackStatus) {
  using codex_auth::detail::classify_device_poll_response;
  using codex_auth::detail::poll_response_e;
  EXPECT_EQ(classify_device_poll_response(403, R"({"error":"slow_down"})"), poll_response_e::slow_down);
  EXPECT_EQ(classify_device_poll_response(403, R"({"error":"deviceauth_authorization_pending"})"),
            poll_response_e::pending);
  EXPECT_EQ(classify_device_poll_response(400, R"({"error":{"code":"deviceauth_authorization_pending"}})"),
            poll_response_e::pending);
  EXPECT_EQ(classify_device_poll_response(404, ""), poll_response_e::pending);
  EXPECT_EQ(classify_device_poll_response(200, R"({"authorization_code":"code"})"),
            poll_response_e::authorized);
  EXPECT_EQ(classify_device_poll_response(400, R"({"error":"authorization_declined"})"),
            poll_response_e::error);
}

TEST(CodexAuthTest, StoresAccountSeparatelyFromApiKey) {
  const credential_files_t files;
  const std::string secret = "refresh-token-must-not-appear-on-disk";
  ASSERT_TRUE(credential_store::write_llm_api_key(files.api_key, "sk-api-key").success);
  ASSERT_TRUE(credential_store::write_codex_credential(files.codex, secret).success);

  const auto stored = credential_store::read_codex_credential(files.codex);
  ASSERT_EQ(stored.status, credential_store::read_status_e::success) << stored.error;
  EXPECT_EQ(stored.secret, secret);
  EXPECT_EQ(credential_store::read_llm_api_key(files.codex).status,
            credential_store::read_status_e::error);
  std::ifstream file(files.codex, std::ios::binary);
  const std::string on_disk(std::istreambuf_iterator<char> { file }, {});
  EXPECT_EQ(on_disk.find(secret), std::string::npos);
  file.close();

  ASSERT_TRUE(credential_store::erase_codex_credential(files.codex).success);
  EXPECT_FALSE(std::filesystem::exists(files.codex));
  EXPECT_EQ(credential_store::read_llm_api_key(files.api_key).secret, "sk-api-key");
}

TEST(CodexAuthTest, LoadsStoredTokenAndLogsOut) {
  const credential_files_t files;
  const auto expiry = std::chrono::duration_cast<std::chrono::seconds>(
    std::chrono::system_clock::now().time_since_epoch()).count() + 3600;
  const auto serialized = nlohmann::json {
    { "version", 1 },
    { "access_token", test_access_token },
    { "refresh_token", "refresh-test" },
    { "account_id", "acct_test" },
    { "expires_at", expiry }
  }.dump();
  ASSERT_TRUE(credential_store::write_codex_credential(files.codex, serialized).success);

  const auto state = codex_auth::status(files.codex);
  ASSERT_TRUE(state.connected) << state.error;
  EXPECT_FALSE(state.pending);
  EXPECT_EQ(state.account_id, "acct_test");
  const auto token = codex_auth::access_token(files.codex);
  ASSERT_TRUE(token.success) << token.error;
  EXPECT_EQ(token.access_token, test_access_token);
  EXPECT_EQ(token.account_id, "acct_test");

  ASSERT_TRUE(codex_auth::logout(files.codex).success);
  EXPECT_FALSE(codex_auth::status(files.codex).connected);
}

TEST(CodexAuthTest, RejectsInvalidStoredAccountWithoutLeakingIt) {
  const credential_files_t files;
  ASSERT_TRUE(credential_store::write_codex_credential(files.codex, "secret-invalid-json").success);
  const auto state = codex_auth::status(files.codex);
  EXPECT_FALSE(state.connected);
  EXPECT_EQ(state.error.find("secret-invalid-json"), std::string::npos);
  const auto token = codex_auth::access_token(files.codex);
  EXPECT_FALSE(token.success);
  EXPECT_TRUE(token.access_token.empty());
}
#endif
