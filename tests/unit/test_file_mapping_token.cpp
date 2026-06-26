/**
 * @file tests/unit/test_file_mapping_token.cpp
 * @brief Test src/file_mapping_token.*.
 */
#include <src/file_mapping_token.h>

#include <gtest/gtest.h>

TEST(FileMappingToken, IssuesSingleUseTokens) {
  file_mapping_token::token_store_t store;

  auto token = store.issue("client-uuid");
  EXPECT_EQ(token.size(), 64);
  auto client_uuid = store.consume(token);
  ASSERT_TRUE(client_uuid.has_value());
  EXPECT_EQ(*client_uuid, "client-uuid");
  EXPECT_FALSE(store.consume(token).has_value());
}

TEST(FileMappingToken, ExpiresTokens) {
  using store_t = file_mapping_token::token_store_t;

  store_t store { std::chrono::seconds { 1 } };
  const auto now = store_t::clock_t::now();
  auto token = store.issue("client-uuid", now);

  EXPECT_FALSE(store.consume(token, now + std::chrono::seconds { 2 }).has_value());
  EXPECT_EQ(store.size(), 0);
}
