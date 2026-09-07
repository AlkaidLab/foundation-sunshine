/**
 * @file tests/unit/test_http_util.cpp
 * @brief Tests for shared HTTP request validation helpers.
 */
#include <gtest/gtest.h>

#include <array>
#include <map>
#include <sstream>

#include <src/http_util.h>

using namespace std::literals;

TEST(HttpUtilTest, MatchesNormalizedMediaType) {
  EXPECT_TRUE(http_util::content_type_matches(" Application/JSON; charset=utf-8", "application/json"));
  EXPECT_TRUE(http_util::content_type_matches("text/plain", "TEXT/PLAIN"));
  EXPECT_FALSE(http_util::content_type_matches("text/plain", "application/json"));
}

TEST(HttpUtilTest, LogsOnlyAllowedRequestFields) {
  const std::multimap<std::string, std::string> fields {
    { "appid", "123" },
    { "clientname", "Living room" },
    { "mode", "1920x1080x60\r\nforged" },
    { "rikey", "secret" },
    { "uniqueid", "client-01" },
  };
  static constexpr std::array allowed_names {
    "appid"sv,
    "clientname"sv,
    "mode"sv,
    "uniqueid"sv,
  };
  std::ostringstream output;

  http_util::append_allowed_request_log_fields(output, ", PARAMS: "sv, fields, allowed_names, "&"sv);

  EXPECT_EQ(output.str(), ", PARAMS: appid=123&clientname=Living room&mode=1920x1080x60??forged&uniqueid=client-01");
}
