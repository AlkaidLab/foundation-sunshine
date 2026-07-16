/**
 * @file tests/unit/test_http_util.cpp
 * @brief Tests for shared HTTP request validation helpers.
 */
#include <gtest/gtest.h>

#include <src/http_util.h>

TEST(HttpUtilTest, MatchesNormalizedMediaType) {
  EXPECT_TRUE(http_util::content_type_matches(" Application/JSON; charset=utf-8", "application/json"));
  EXPECT_TRUE(http_util::content_type_matches("text/plain", "TEXT/PLAIN"));
  EXPECT_FALSE(http_util::content_type_matches("text/plain", "application/json"));
}
